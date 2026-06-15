/*---------------------------------------------------------------------------
    dd_sysinfo.cpp -- board sensors + disk free.

    Temps/fan via HalReadSMBusValue (kernel SMBus), ported from XbDiag:
      ADM1032 @ 0x98 (rev 1.0-1.5): reg 0x01 = CPU (remote), 0x00 = board
      PIC/SMC @ 0x20 (rev 1.6 / fallback): reg 0x09 = CPU, 0x0A = board
      PIC/SMC @ 0x20 reg 0x10 = fan, raw 0..50 -> *2 = 0..100%
    Sys_Init() does the nForce SMBus W1C reset (0xC000) so a stuck transaction
    left by a softmod payload can't make HalReadSMBusValue spin forever.
    Disk free via GetDiskFreeSpaceExA (ported from HddInfo formatting).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <string.h>
#include "xboxinternals.h"
#include "dd_sysinfo.h"
#include "dd_watchdog.h"
#include "dd_smbus.h"          /* the single SMBus owner -- all access goes here now */
#include "dd_rtc.h"            /* X-RTC mirror (no-op when absent) */

#define SMBADDR_PIC      0x20   /* PIC16/SMC          (software-shifted 8-bit) */
#define SMBADDR_ADM1032  0x98   /* ADM1032 temp mon   (software-shifted 8-bit) */

/* SMBus access now goes through the broker (dd_smbus). These thin wrappers keep
   the rest of this file unchanged and label the turn "SYS" for the watchdog
   breadcrumb. The broker does the preflight, the inter-master guard, the
   serialization against the LCD writer / watchdog thread, and the consecutive-
   fail self-reset -- all the things these helpers used to do inline, now in one
   place so every consoler takes its turn on the bus. */
static int SMBusRead(BYTE addr, BYTE reg, BYTE* out) {
    int ok;
    Smb_BeginTurn("SYS");
    ok = Smb_Read8(addr, reg, out);
    Smb_EndTurn();
    return ok;
}

static int SMBusWrite(BYTE addr, BYTE reg, BYTE val) {
    int ok;
    Smb_BeginTurn("SYS");
    ok = Smb_Write8(addr, reg, val);
    Smb_EndTurn();
    return ok;
}

/* Kernel-arbitrated PCI config access (safer than raw 0xCF8/0xCFC port I/O,
   which shares the config-address register with the kernel). SlotNumber packs
   dev[4:0]<<5 | func[2:0] -- the PCI_SLOT_NUMBER format. */
extern "C" VOID __stdcall HalReadWritePCISpace(
    ULONG BusNumber, ULONG SlotNumber, ULONG RegisterNumber,
    PVOID Buffer, ULONG Length, BOOLEAN WritePCISpace);

static DWORD PciRead32(BYTE bus, BYTE dev, BYTE func, BYTE reg) {
    DWORD val = 0;
    ULONG slot = (((ULONG)dev & 0x1F) << 5) | ((ULONG)func & 0x07);
    HalReadWritePCISpace(bus, slot, reg, &val, sizeof(val), FALSE);
    return val;
}

/* Xbox reference crystal: 16.666... MHz, feeds both the CPUMPLL FSB and the
   NV2A NVPLL. Written as the repeating decimal to match the reference impls. */
static const double XTAL_HZ = 16666666.6667;

void Sys_SmbusReset(void) {
    /* Kept for the existing callers (e.g. RevisionDetect probes before reading).
       The W1C lives in the broker now; this is the lock-free emergency form,
       which is fine here because callers invoke it between turns, not mid-turn. */
    Smb_EmergencyReset();
}

void Sys_Init(void) {
    Smb_Init();                /* stand up the bus lock + clear the controller once */
}

/* Runtime SMBus self-heal now lives in the broker (dd_smbus NoteResult): a RUN
   of consecutive transaction failures W1Cs the controller from inside the lock.
   This shim stays so the temp-read call sites below don't have to change; it is
   a no-op because the broker already counted the result of every transaction. */
static void SmbusWatchdog(int ok) {
    (void)ok;
}

/* All callers share one cached reading so the three independent consumers (status
   scroll, Type-D telemetry, LCD sensor page) don't each hit the shared SMBus --
   that uncoordinated, unthrottled traffic was the main-thread bus load left over
   after the LCD was paced to 1Hz.
   Temps and fan move slowly, and every read hits the SMC (PIC 0x20) -- the same
   slow microcontroller Cerbios polls for its own fan/thermal control. Unlike
   XbDiag (a live diagnostic that wants to-the-second readings), a dashboard just
   needs recent ambient context, so we read GENTLY: each sensor every 15s, and
   staggered so temp and fan never read on the same cycle. Net: one small read
   burst every ~7.5s instead of a cluster every second. */
#define TEMP_CACHE_MS  15000   /* per-sensor refresh interval */
#define SENSOR_STAGGER_MS (TEMP_CACHE_MS / 2)   /* offset fan from temp by half */
static DWORD s_tempTick = 0;
static int   s_tempOk = 0;
static int   s_tempCpu = 0;
static int   s_tempBoard = 0;

/* Which temp source works on this box: 0 = unknown (try ADM1032 then SMC),
   1 = ADM1032, 2 = SMC. Latched after the first success so we never again fire
   doomed reads at an absent device (a 1.6/Xyclops has no ADM1032 -- probing it
   every cycle is two failed transactions that just lean on the bus). */
static int s_tempPath = 0;

static int ReadAdm(int* cpuC, int* boardC) {
    BYTE c = 0, b = 0;
    if (SMBusRead(SMBADDR_ADM1032, 0x01, &c) && SMBusRead(SMBADDR_ADM1032, 0x00, &b)) {
        if (cpuC) *cpuC = c; if (boardC) *boardC = b; return 1;
    }
    return 0;
}
static int ReadPic(int* cpuC, int* boardC) {
    BYTE c = 0, b = 0;
    if (SMBusRead(SMBADDR_PIC, CPU_TEMP, &c) && SMBusRead(SMBADDR_PIC, MB_TEMP, &b)) {
        b = (BYTE)((int)b * 4 / 5);          /* 1.6 board-temp correction */
        if (cpuC) *cpuC = c; if (boardC) *boardC = b; return 1;
    }
    return 0;
}

static int ReadTempsRaw(int* cpuC, int* boardC) {
    /* Once we know the source, read ONLY it -- no fallback probing of the dead
       device. ADM1032 (rev 1.0-1.5) regs read directly; PIC/SMC (1.6/Xyclops)
       at CPU_TEMP (0x09)/MB_TEMP (0x0A) with the 0.8x board correction. */
    if (s_tempPath == 1) {
        if (ReadAdm(cpuC, boardC)) { SmbusWatchdog(1); return 1; }
        s_tempPath = 0;                       /* lost it -- re-learn next time     */
    }
    else if (s_tempPath == 2) {
        if (ReadPic(cpuC, boardC)) { SmbusWatchdog(1); return 1; }
        s_tempPath = 0;
    }
    else {
        if (ReadAdm(cpuC, boardC)) { s_tempPath = 1; SmbusWatchdog(1); return 1; }
        if (ReadPic(cpuC, boardC)) { s_tempPath = 2; SmbusWatchdog(1); return 1; }
    }
    SmbusWatchdog(0);
    return 0;
}

int Sys_ReadTemps(int* cpuC, int* boardC) {
    /* Cache-only: returns the values the SMBus service thread last published.
       Never touches the bus (the main/render thread must not). The actual read
       happens in Sys_ServiceSensors() on the service thread. */
    if (cpuC)   *cpuC = s_tempCpu;
    if (boardC) *boardC = s_tempBoard;
    return s_tempOk;
}

/* Fan-read cache + cold-boot retry state (file scope, per project convention).
   Right after a (soft) reboot the first FAN_READBACK samples can come back bad:
   0x00 because the SMC PIC is still settling, or garbage because the Type-D
   telemetry unit is mid-transaction on the shared SMBus. A 0x00 sails straight
   through the 0..50 range check as a bogus "0%". So: cache the last good reading,
   and until we've seen one, back off briefly and gently re-poll. Once a valid
   sample latches, the slow path is never taken again (no hitches in normal use,
   and a genuinely absent sensor can't stall the dashboard forever). */
static int s_fanHaveGood = 0;   /* 1 once a valid 1..50 sample has been seen   */
static int s_fanLastGood = 0;   /* last valid reading, already scaled to 0..100 */
static int s_fanColdTries = 0;   /* cold-boot retry sequences spent (capped)     */
static DWORD s_fanTick = 0;   /* GetTickCount of last good sample (freshness)  */

int Sys_ReadFanPct(int* pct) {
    /* Cache-only: last value published by the SMBus service thread. No bus. */
    if (pct) *pct = s_fanLastGood;
    return s_fanHaveGood;
}

/* Bus-side fan read (service thread only). Fast path: one read, valid speed is
   1..50 (== 2..100%); 0x00 / out-of-range is a bad sample, not a real stop.
   On a cold/soft boot with no good reading yet, gently re-poll (capped) so an
   absent sensor can't retry forever. The Sleep()s here run on the service
   thread, never the render thread. */
static void ServiceFan(DWORD now) {
    BYTE f = 0;
    /* Single gentle read. A valid speed is 1..50 (== 2..100%); anything else is
       a bad sample, not a real stop. */
    if (SMBusRead(SMBADDR_PIC, FAN_READBACK, &f) && f >= 1 && f <= 50) {
        s_fanLastGood = (int)f * 2; s_fanHaveGood = 1; s_fanTick = now;
        s_fanColdTries = 0;
        return;
    }
    if (s_fanHaveGood) return;        /* keep last good rather than flash 0% */
    /* No good reading yet (cold boot). Do NOT burst the SMC with Sleep-stalled
       re-reads -- just count this cycle as one attempt and try again on the next
       service cycle (~7.5s later). After a few cold cycles, give up quietly and
       leave the fan unknown rather than keep probing the PIC forever. */
    if (s_fanColdTries < 6) s_fanColdTries++;
}

/* The ONLY place sensor reads touch the SMBus. Called only by the service
   thread (dd_smbsvc). Rate-limited to ~1Hz so the service loop can spin faster
   for the LCD without over-polling the sensors. */
void Sys_ServiceSensors(void) {
    DWORD now = GetTickCount();
    /* Temp and fan are each refreshed every TEMP_CACHE_MS, but offset by half a
       cycle: a given service tick services AT MOST one of them, so the SMC never
       takes two read bursts back to back. */
    if (s_tempTick == 0 || (now - s_tempTick) >= TEMP_CACHE_MS) {
        s_tempOk = ReadTempsRaw(&s_tempCpu, &s_tempBoard);
        s_tempTick = now;
        return;                                  /* fan waits for a later tick */
    }
    if (!(s_fanHaveGood && s_fanTick != 0 && (now - s_fanTick) < TEMP_CACHE_MS)) {
        /* hold the fan read off until we're at least half a cycle past the temp
           read, so the two never land together */
        if (s_fanHaveGood && (now - s_tempTick) < SENSOR_STAGGER_MS) return;
        ServiceFan(now);
    }
}

static void UIntToStr(unsigned v, char* out, int cap) {
    char t[12];
    int  n = 0, i = 0;
    if (cap <= 0) return;
    if (v == 0) { if (cap > 1) { out[0] = '0'; out[1] = 0; } else out[0] = 0; return; }
    while (v > 0 && n < 11) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && i < cap - 1) out[i++] = t[--n];
    out[i] = 0;
}

void Sys_DiskFreeStr(const char* drive, char* out, int cap) {
    ULARGE_INTEGER freeToCaller, total, freeBytes;
    char num[12];
    if (cap <= 0) return;
    out[0] = 0;

    if (!GetDiskFreeSpaceExA(drive, &freeToCaller, &total, &freeBytes)) {
        if (cap > 2) { out[0] = '-'; out[1] = '-'; out[2] = 0; }
        return;
    }
    {
        DWORD freeMB = (DWORD)(freeBytes.QuadPart / (1024ULL * 1024ULL));
        if (freeMB < 1024) {
            UIntToStr(freeMB, num, sizeof(num));
            strncpy(out, num, cap - 1); out[cap - 1] = 0;
            strncat(out, " MB", cap - strlen(out) - 1);
        }
        else {
            DWORD gb = freeMB / 1024;
            DWORD dec = (freeMB % 1024) * 10 / 1024;
            UIntToStr(gb, num, sizeof(num));
            strncpy(out, num, cap - 1); out[cap - 1] = 0;
            strncat(out, ".", cap - strlen(out) - 1);
            UIntToStr(dec, num, sizeof(num));
            strncat(out, num, cap - strlen(out) - 1);
            strncat(out, " GB", cap - strlen(out) - 1);
        }
    }
}

/* append an MB count formatted as "N MB" (<1GB) or "N.N GB" to *out (bounded) */
static void AppendSize(char* out, int cap, DWORD mb) {
    char num[12];
    if (mb < 1024) {
        UIntToStr(mb, num, sizeof(num));
        strncat(out, num, cap - strlen(out) - 1);
        strncat(out, " MB", cap - strlen(out) - 1);
    }
    else {
        DWORD gb = mb / 1024, dec = (mb % 1024) * 10 / 1024;
        UIntToStr(gb, num, sizeof(num));
        strncat(out, num, cap - strlen(out) - 1);
        strncat(out, ".", cap - strlen(out) - 1);
        UIntToStr(dec, num, sizeof(num));
        strncat(out, num, cap - strlen(out) - 1);
        strncat(out, " GB", cap - strlen(out) - 1);
    }
}

/* "free / total" for a drive, e.g. "4.2 / 8.0 GB". "--" if the drive isn't there. */
void Sys_DiskUsageStr(const char* drive, char* out, int cap) {
    ULARGE_INTEGER freeToCaller, total, freeBytes;
    if (cap <= 0) return;
    out[0] = 0;
    if (!GetDiskFreeSpaceExA(drive, &freeToCaller, &total, &freeBytes)) {
        if (cap > 2) { out[0] = '-'; out[1] = '-'; out[2] = 0; }
        return;
    }
    {
        DWORD freeMB = (DWORD)(freeBytes.QuadPart / (1024ULL * 1024ULL));
        DWORD totalMB = (DWORD)(total.QuadPart / (1024ULL * 1024ULL));
        AppendSize(out, cap, freeMB);
        strncat(out, " / ", cap - strlen(out) - 1);
        AppendSize(out, cap, totalMB);
    }
}

/*---------------------------------------------------------------------------
    Fan control -- ported verbatim from XbDiag StressTestCPU's proven model.
    PIC (SMC) at 0x20:
        reg 0x05 = fan mode   (0 = SMC automatic, 1 = custom speed)
        reg 0x06 = fan speed  (0..50, where percent = raw * 2)
    Mode MUST be set to 1 before the speed write or the SMC ignores it.
    Auto / release writes 0 to both, handing control back to the SMC.

    Safety: the dashboard defaults to auto and never writes on boot. A manual
    set still clamps to a floor so a mistaken 0% can't stop the fan dead.
---------------------------------------------------------------------------*/
#define FAN_MIN_MANUAL_PCT 20   /* never let a manual override drop below this */

int Sys_FanAuto(void) {
    int ok = SMBusWrite(SMBADDR_PIC, 0x05, 0);   /* mode = automatic */
    ok = SMBusWrite(SMBADDR_PIC, 0x06, 0) && ok;
    return ok;                                    /* 0 if the bus refused (e.g. xemu) */
}

int Sys_FanSetManual(int pct) {
    BYTE raw;
    int  ok;
    if (pct < FAN_MIN_MANUAL_PCT) pct = FAN_MIN_MANUAL_PCT;
    if (pct > 100) pct = 100;
    raw = (BYTE)(pct / 2);              /* 0..50 */
    ok = SMBusWrite(SMBADDR_PIC, 0x05, 1);        /* mode = custom (before speed!) */
    ok = SMBusWrite(SMBADDR_PIC, 0x06, raw) && ok;
    return ok;
}

/*---------------------------------------------------------------------------
    Power control via the SMC PIC, the same way XbDiag does it (and the reason
    its web UI reboots/shuts down reliably). HalReturnToFirmware's REBOOT/HALT
    paths are flaky on modded boxes -- HALT in particular doesn't actually cut
    power on most BIOSes -- so we poke the SMC command register directly:

        SMBus 0x20, reg 0x02:  0x01 = warm reset,  0x40 = power cycle,
                               0x80 = power off

    The SMC acts a few dozen ms after the write, so we spin briefly afterwards
    rather than letting the dashboard render another frame. If the bus refuses
    the write (e.g. xemu, which has no real SMC), we fall back to the firmware
    path so the action still happens under emulation.
---------------------------------------------------------------------------*/
#define SMC_REG_POWER   0x02
#define SMC_PWR_RESET   0x01
#define SMC_PWR_CYCLE   0x40
#define SMC_PWR_OFF     0x80

static void Sys_SmcPower(BYTE cmd, unsigned int fwFallback) {
    if (SMBusWrite(SMBADDR_PIC, SMC_REG_POWER, cmd)) {
        int i;
        for (i = 0; i < 20; i++) Sleep(100);   /* ~2s; the box dies here on real HW */
    }
    HalReturnToFirmware(fwFallback);           /* emulator / refused-write fallback */
}

void Sys_Reset(void) { Sys_SmcPower(SMC_PWR_RESET, RETURN_FIRMWARE_REBOOT); }
void Sys_PowerCycle(void) { Sys_SmcPower(SMC_PWR_CYCLE, RETURN_FIRMWARE_REBOOT); }
void Sys_PowerOff(void) { Sys_SmcPower(SMC_PWR_OFF, RETURN_FIRMWARE_HALT); }

/*---------------------------------------------------------------------------
    Installed RAM, megabytes. GlobalMemoryStatus reports total physical;
    round to the nearest 64 so a 64MB box reads 64 and a 128MB mod reads 128.
---------------------------------------------------------------------------*/
int Sys_RamMB(void) {
    MEMORYSTATUS ms;
    DWORD mb;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    mb = (DWORD)(ms.dwTotalPhys / (1024UL * 1024UL));
    if (mb >= 96)  return 128;
    return 64;
}

/* free physical RAM right now, in MB (rounds down). */
int Sys_RamFreeMB(void) {
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    return (int)(ms.dwAvailPhys / (1024UL * 1024UL));
}

/*---------------------------------------------------------------------------
    First-order Xbox revision from the video encoder on the SMBus -- the
    reliable, lightweight method (full 1.6 vs 1.6b split needs NV2A straps,
    which we skip here):
        Conexant CX25871  @ 0x8A  -> 1.0 - 1.1
        Focus FS454       @ 0xD4  -> 1.2 - 1.5
        Xcalibur          @ 0xE0  -> 1.6 / 1.6b
    Returns a short human string; "Unknown" if none answer.
---------------------------------------------------------------------------*/
static const char* RevisionDetect(void) {
    BYTE c0 = 0, c1 = 0, c2 = 0, enc = 0;
    char ver[4];

    /* The broker preflights (clears a dirty controller) at the start of every
       turn, so the PIC reads below already start clean -- no explicit lock-free
       reset here (it could W1C the controller while the service thread is
       mid-transaction). */

       /* The SMC/PIC reports its build string one character per read of reg 0x01,
          cycling its internal pointer -- so the three chars can arrive at any
          rotation. Read three, then match every rotation of each known pattern.
          (method: PrometheOS xboxConfig::getXboxVersion) */
    SMBusRead(SMBADDR_PIC, 0x01, &c0);
    SMBusRead(SMBADDR_PIC, 0x01, &c1);
    SMBusRead(SMBADDR_PIC, 0x01, &c2);
    ver[0] = (char)c0; ver[1] = (char)c1; ver[2] = (char)c2; ver[3] = 0;

    if (!strcmp(ver, "01D") || !strcmp(ver, "D01") ||
        !strcmp(ver, "1D0") || !strcmp(ver, "0D1")) return "DevKit";
    if (!strcmp(ver, "DBG") || !strcmp(ver, "B11")) return "DebugKit";
    if (!strcmp(ver, "P01")) return "1.0";
    if (!strcmp(ver, "P05")) return "1.1";
    if (!strcmp(ver, "P11") || !strcmp(ver, "1P1") || !strcmp(ver, "11P")) {
        /* 1.2/1.3 share the PIC string with 1.4/1.5; the Focus encoder (0xD4)
           only exists on 1.4/1.5, so its presence disambiguates. */
        if (SMBusRead(0xD4, 0x00, &enc)) return "1.4 / 1.5";
        return "1.2 / 1.3";
    }
    if (!strcmp(ver, "P2L")) {
        /* 1.6 vs 1.6b: NV2A RAM strap (EMRS) bits 18-19 == 3 -> Hynix (1.6b),
           else Samsung (1.6). */
        unsigned long strap = ((*((volatile unsigned long*)0xFD101000)) & 0x000C0000UL) >> 18;
        return (strap == 3) ? "1.6b" : "1.6";
    }

    /* PIC string unrecognized (e.g. an unusual SMC) -- fall back to the encoder
       heuristic so we still report a sensible range. */
    if (SMBusRead(0x8A, 0x00, &enc)) return "1.0 - 1.1 (Conexant)";
    if (SMBusRead(0xD4, 0x00, &enc)) return "1.2 - 1.5 (Focus)";
    if (SMBusRead(0xE0, 0x00, &enc)) return "1.6 (Xcalibur)";
    return "Unknown";
}

/* These hardware facts never change at runtime, and the detect paths touch the
   SMBus (incl. a settling stall), so compute once and cache -- the About screen
   rebuilds its text every frame and must not re-probe (or stall) each time. */
const char* Sys_XboxRevision(void) {
    static const char* s_rev = 0;
    if (!s_rev) {
        /* The console version is immutable -- read it EXACTLY ONCE and latch the
           result for the life of the process, so no caller (service thread OR the
           About screen) can ever re-probe the SMC for it. Don't latch before the
           bus is live, or we'd cache a boot-settle "Unknown" forever; once it's
           ready, whatever the single read returns is final. */
        if (!Smb_Ready()) return "Unknown";
        s_rev = RevisionDetect();
    }
    return s_rev;
}

/* ---- CPU / GPU clock (PLL-based; reads the real rate, OC included) --------
   The displayed speed used to be a hardcoded "733 MHz", so an overclock never
   showed. These read the actual PLL configuration instead of assuming stock.
   (method: XbDiag SysInfo / StressTestCPU, which matches PrometheOS.) */

   /* MSR 0x2A bits [27:22] (masked 0x2F) = the CPU core ratio index the CPU writes
      during init -- authoritative even on Tualatin upgrades where the bootloader
      CPUCTL value may be stale. Returns ratio x10 (e.g. 55 == 5.5x), 0 if unknown. */
static DWORD CpuRatioX10FromMsr(DWORD msr_lo) {
    BYTE pat = (BYTE)((msr_lo >> 22) & 0x2F);
    switch (pat) {
    case 0x01: return 30;  case 0x05: return 35;  case 0x02: return 40;
    case 0x06: return 45;  case 0x00: return 50;  case 0x04: return 55;
    case 0x0B: return 60;  case 0x0F: return 65;  case 0x09: return 70;
    case 0x0D: return 75;  case 0x0A: return 80;  case 0x26: return 85;
    case 0x20: return 90;  case 0x24: return 95;  case 0x2B: return 100;
    case 0x2F: return 105; case 0x2A: return 130; case 0x2C: return 140;
    default:   return 0;
    }
}

static DWORD CpuMHzDetect(void) {
    /* CPUMPLL (PCI 0:3:0 offset 0x6C): byte0 = FSB divider, byte1 = FSB mult.
       FSB = XTAL * mult/div; CPU = FSB * ratio. */
    DWORD cpumpll = PciRead32(0, 3, 0, 0x6C);
    DWORD fsb_div = cpumpll & 0xFF;
    DWORD fsb_mult = (cpumpll >> 8) & 0xFF;
    DWORD msr_lo = 0, ratio, result;
    double fsb_hz, cpu_mhz;

    if (fsb_div == 0 || fsb_mult == 0) return 733;
    fsb_hz = XTAL_HZ * ((double)fsb_mult / (double)fsb_div);

    __asm {
        mov  ecx, 0x2A
        rdmsr
        mov  msr_lo, eax
    }
    ratio = CpuRatioX10FromMsr(msr_lo);
    if (ratio == 0) return 733;

    cpu_mhz = (fsb_hz * ((double)ratio / 10.0)) / 1.0e6;
    result = (DWORD)(cpu_mhz + 0.5);
    if (result < 400 || result > 1600) return 733;   /* implausible -> stock */
    return result;
}

static DWORD GpuMHzDetect(void) {
    /* NV2A PRAMDAC NVPLL at MMIO 0xFD680500: M=bits[7:0], N=bits[15:8],
       P=bits[18:16]. F = (XTAL * N / 2^P) / M. */
    volatile DWORD* pll;
    DWORD reg, M, N, P, mhz;
    double gpu_hz;

    /* confirm the NV2A is really there before dereferencing its MMIO window */
    if ((PciRead32(0, 0, 0, 0x00) & 0xFFFF) != 0x10DE) return 233;

    pll = (volatile DWORD*)(0xFD000000UL + 0x00680500UL);
    reg = *pll;
    M = (reg >> 0) & 0xFF;
    N = (reg >> 8) & 0xFF;
    P = (reg >> 16) & 0x07;
    if (M == 0 || N == 0) return 233;

    gpu_hz = ((double)N * XTAL_HZ / (double)(1u << P)) / (double)M;
    mhz = (DWORD)(gpu_hz / 1.0e6 + 0.5);
    if (mhz < 150 || mhz > 400) return 233;          /* implausible -> stock */
    return mhz;
}

DWORD Sys_CpuMHz(void) {
    static DWORD s_mhz = 0;             /* fixed at runtime: detect once, cache */
    if (!s_mhz) s_mhz = CpuMHzDetect();
    return s_mhz;
}

DWORD Sys_GpuMHz(void) {
    static DWORD s_mhz = 0;
    if (!s_mhz) s_mhz = GpuMHzDetect();
    return s_mhz;
}

/*---------------------------------------------------------------------------
    System clock (runtime, no EEPROM). The Xbox keeps time in UTC; the kernel
    applies the EEPROM timezone bias when converting to local. We read/set
    LOCAL time and let the Win32 conversions handle the bias:
        get: KeQuerySystemTime(UTC) -> FileTimeToLocalFileTime -> FileTimeToSystemTime
        set: SystemTimeToFileTime(local) -> LocalFileTimeToFileTime(UTC) -> NtSetSystemTime
    NtSetSystemTime returns NTSTATUS (0 = success). An invalid date (e.g. Feb 31)
    makes SystemTimeToFileTime fail, so Sys_SetClock just returns 0 -- no crash.
    Mechanism confirmed against the uploaded rtcManager reference.
---------------------------------------------------------------------------*/
void Sys_GetClock(SysClock* c) {
    FILETIME   utc, local;
    SYSTEMTIME st;
    if (!c) return;
    KeQuerySystemTime(&utc);
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &st);
    c->year = st.wYear;  c->mon = st.wMonth;  c->day = st.wDay;
    c->hour = st.wHour;  c->min = st.wMinute; c->sec = st.wSecond;
    c->dow = st.wDayOfWeek;
}

int Sys_SetClock(const SysClock* c) {
    SYSTEMTIME st;
    FILETIME   local, utc;
    if (!c) return 0;
    ZeroMemory(&st, sizeof(st));
    st.wYear = (WORD)c->year;  st.wMonth = (WORD)c->mon;  st.wDay = (WORD)c->day;
    st.wHour = (WORD)c->hour;  st.wMinute = (WORD)c->min;  st.wSecond = (WORD)c->sec;
    st.wMilliseconds = 0;
    if (!SystemTimeToFileTime(&st, &local))      return 0;   /* invalid date */
    if (!LocalFileTimeToFileTime(&local, &utc))  return 0;
    {
        int ok = (NtSetSystemTime(&utc, NULL) == 0);         /* 0 = STATUS_SUCCESS */
        if (ok) Rtc_WriteUtc(&utc);                          /* persist to X-RTC (no-op if absent) */
        return ok;
    }
}

/* Set the clock from already-resolved wall-clock fields WITHOUT going through
   LocalFileTimeToFileTime (which would re-apply the EEPROM timezone bias). NTP
   sync uses this: it computes local time itself from UTC + the user's stored
   offset, so we must not let the kernel's EEPROM-TZ conversion touch it again.
   The fields are written straight as the system time. */
int Sys_SetClockDirect(const SysClock* c) {
    SYSTEMTIME st;
    FILETIME   ft;
    if (!c) return 0;
    ZeroMemory(&st, sizeof(st));
    st.wYear = (WORD)c->year;  st.wMonth = (WORD)c->mon;  st.wDay = (WORD)c->day;
    st.wHour = (WORD)c->hour;  st.wMinute = (WORD)c->min;  st.wSecond = (WORD)c->sec;
    st.wMilliseconds = 0;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;           /* invalid date */
    {
        int ok = (NtSetSystemTime(&ft, NULL) == 0);
        if (ok) Rtc_WriteUtc(&ft);                           /* persist to X-RTC (no-op if absent) */
        return ok;
    }
}

/* One-shot boot restore: if an X-RTC is present, read its UTC and seed the kernel
   system clock from it. Cerbios does not seed the clock from the X-RTC, so the
   dashboard makes the X-RTC authoritative -- loaded here at boot, updated on every
   Set/NTP. UTC goes straight in (the timezone is applied at display). No-op and
   harmless if absent, or if Cerbios already happened to seed the same value.
   Returns 1 if it seeded. */
int Sys_SeedFromRtc(void) {
    FILETIME ft;
    if (!Rtc_Present())     return 0;
    if (!Rtc_ReadUtc(&ft))  return 0;
    return NtSetSystemTime(&ft, NULL) == 0;
}