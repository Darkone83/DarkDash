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

#define SMBADDR_PIC      0x20   /* PIC16/SMC          (software-shifted 8-bit) */
#define SMBADDR_ADM1032  0x98   /* ADM1032 temp mon   (software-shifted 8-bit) */

static int SMBusRead(BYTE addr, BYTE reg, BYTE* out) {
    DWORD v = 0;
    *out = 0;
    if (HalReadSMBusValue(addr, reg, FALSE, &v) != 0) return 0;
    *out = (BYTE)(v & 0xFF);
    return 1;
}

static int SMBusWrite(BYTE addr, BYTE reg, BYTE val) {
    return HalWriteSMBusValue(addr, reg, FALSE, (DWORD)val) == 0;
}

void Sys_Init(void) {
    /* W1C-clear the nForce SMBus global status, releasing any stuck/in-progress
       transaction (softmod payloads can leave one pending). Without this the
       kernel's SMBus retry loop can spin forever -> dashboard hang. No-op when
       the controller is already idle. */
    __asm {
        mov dx, 0xC000
        mov al, 0xFF
        out dx, al
    }
    KeStallExecutionProcessor(2000);   /* let the bus settle (PIC/ADM <100us) */
}

int Sys_ReadTemps(int* cpuC, int* boardC) {
    BYTE c = 0, b = 0;
    /* ADM1032 (rev 1.0-1.5): regs read directly, no scaling. */
    if (SMBusRead(SMBADDR_ADM1032, 0x01, &c) && SMBusRead(SMBADDR_ADM1032, 0x00, &b)) {
        if (cpuC) *cpuC = c; if (boardC) *boardC = b; return 1;
    }
    /* PIC/SMC fallback (rev 1.6 / Xyclops): reg 0x09 = CPU, 0x0A = board.
       On 1.6 the board reading runs high and needs the 0.8x ambient scaling
       that the ADM1032 path doesn't (ref: PrometheOS xboxConfig). The 1.0-1.5
       PIC proxy returns the same value the ADM1032 would, so scaling it would
       be wrong there -- but this branch is only reached when the ADM1032 is
       absent, i.e. a 1.6, so applying the scale here is correct. */
    if (SMBusRead(SMBADDR_PIC, 0x09, &c) && SMBusRead(SMBADDR_PIC, 0x0A, &b)) {
        b = (BYTE)((int)b * 4 / 5);          /* 1.6 board-temp correction */
        if (cpuC) *cpuC = c; if (boardC) *boardC = b; return 1;
    }
    return 0;
}

int Sys_ReadFanPct(int* pct) {
    BYTE f = 0;
    if (!SMBusRead(SMBADDR_PIC, 0x10, &f)) return 0;   /* 0..50 */
    if (pct) { int p = (int)f * 2; if (p > 100) p = 100; *pct = p; }
    return 1;
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

/*---------------------------------------------------------------------------
    First-order Xbox revision from the video encoder on the SMBus -- the
    reliable, lightweight method (full 1.6 vs 1.6b split needs NV2A straps,
    which we skip here):
        Conexant CX25871  @ 0x8A  -> 1.0 - 1.1
        Focus FS454       @ 0xD4  -> 1.2 - 1.5
        Xcalibur          @ 0xE0  -> 1.6 / 1.6b
    Returns a short human string; "Unknown" if none answer.
---------------------------------------------------------------------------*/
const char* Sys_XboxRevision(void) {
    BYTE v;
    if (SMBusRead(0x8A, 0x00, &v)) return "1.0 - 1.1 (Conexant)";
    if (SMBusRead(0xD4, 0x00, &v)) return "1.2 - 1.5 (Focus)";
    if (SMBusRead(0xE0, 0x00, &v)) return "1.6 (Xcalibur)";
    return "Unknown";
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
    return NtSetSystemTime(&utc, NULL) == 0;                 /* 0 = STATUS_SUCCESS */
}