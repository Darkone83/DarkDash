/*---------------------------------------------------------------------------
    dd_smbus.cpp -- see dd_smbus.h.

    Build: MSVC2003/C89 style; file-scope statics; no CRT str*; one __asm
    instruction per line. Compiles with /GL like every other DarkDash object.

    This file is the ONLY place HalReadSMBusValue / HalWriteSMBusValue are
    called. It does NOT touch the nForce SMBus status port (0xC000) directly --
    the kernel owns that controller and we go through Hal exclusively. Everything
    else asks the broker.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"     /* HalRead/WriteSMBusValue */
#include "dd_smbus.h"
#include "dd_watchdog.h"       /* Watchdog_SetPhase */
#include "dd_trace.h"

#define SMB_STS_OK        0x10   /* nForce status: last transaction completed OK */
#define SMB_DEF_TURN_GAP  2500   /* inter-master guard after each turn (us)      */
#define SMB_DEF_FAILRESET 4      /* consecutive fails before self W1C            */
#define SMB_SETTLE_US     2000   /* post-W1C settle (PIC/ADM recover <100us)     */
#define SMB_DEF_BOOT_SETTLE_MS 3000 /* hold all kernel SMBus traffic this long after init */

/* ---- broker state (all mutated only while holding s_cs) ------------------- */
static CRITICAL_SECTION s_cs;
static int   s_inited = 0;
static int   s_depth = 0;            /* current turn nesting depth          */
static int   s_fails = 0;            /* consecutive transaction failures    */
static unsigned s_turnGapUs = SMB_DEF_TURN_GAP;
static int   s_failReset = SMB_DEF_FAILRESET;
static DWORD s_bootTick = 0;                       /* GetTickCount at Smb_Init   */
static unsigned s_bootSettleMs = SMB_DEF_BOOT_SETTLE_MS;

/* ---- controller discipline -------------------------------------------------
   We touch the SMBus ONLY through the kernel's HalReadSMBusValue /
   HalWriteSMBusValue. We do NOT read or write the nForce status port (0xC000)
   directly. The kernel owns that controller: it polls 0xC000 internally and
   serializes its OWN SMBus use (SMC, fan, temp, eject) against ours. A direct
   W1C from here can land in the middle of one of the kernel's transactions,
   clear the status it is polling, and send the next HalRead/Write into a
   raised-IRQL spin that never returns -- the hard lock with no bugcheck. So the
   broker is purely a polite, serialized Hal CALLER (lock + boot settle +
   inter-turn spacing) and never manages the controller itself. This is exactly
   how XBMC4Gamers drives the bus, and why it never wedges. */

   /* Track a run of transaction failures (diagnostic only). We do NOT W1C the
      controller on a run -- poking 0xC000 behind the kernel's back is what caused
      the wedges. A genuinely stuck controller is the kernel's to recover; we retry
      through Hal on the next turn. */
static void NoteResult(int ok) {
    if (ok) { s_fails = 0; return; }
    ++s_fails;
}

/* ---- public API ----------------------------------------------------------- */
void Smb_Init(void) {
    if (s_inited) return;
    InitializeCriticalSection(&s_cs);
    s_inited = 1;
    s_bootTick = GetTickCount();   /* start the boot-settle window now */
    /* No controller poke here. The kernel manages the SMBus controller; we just
       hold our own traffic for the boot-settle window, then go through Hal. */
}

/* 1 while we're still inside the post-init settle window (no kernel SMBus
   traffic allowed yet). Safe before init: treat as still-settling. */
static int BootSettling(void) {
    if (!s_inited) return 1;
    return ((DWORD)(GetTickCount() - s_bootTick) < s_bootSettleMs) ? 1 : 0;
}

void Smb_BeginTurn(const char* owner) {
    /* Defensive: if a caller reaches the bus before Sys_Init(), stand the lock
       up now rather than fault. Init is idempotent. */
    if (!s_inited) Smb_Init();
    EnterCriticalSection(&s_cs);
    if (++s_depth == 1) {
        Watchdog_SetPhase(owner ? owner : "SMB");
    }
}

void Smb_EndTurn(void) {
    if (s_depth == 1) {
        /* Leave the bus quiet before anyone else's turn (ours or a secondary
           master's). This non-yielding guard is the inter-master window. */
        KeStallExecutionProcessor(s_turnGapUs);
        Watchdog_SetPhase("main");
    }
    if (s_depth > 0) --s_depth;
    LeaveCriticalSection(&s_cs);
}

int Smb_Read8(unsigned char addr8, unsigned char reg, unsigned char* out) {
    DWORD v = 0;
    int   ok;
    if (out) *out = 0;
    if (!s_inited) Smb_Init();
    if (BootSettling()) return 0;         /* hold off the bus until it has settled */
    Smb_BeginTurn("SMB");                 /* nested no-op inside an explicit turn */
    TRACEU("smb.rd>", "addr/reg", ((unsigned long)addr8 << 8) | reg);
    ok = (HalReadSMBusValue(addr8, reg, FALSE, &v) == 0);
    TRACEU("smb.rd<", "ok", (unsigned long)ok);
    NoteResult(ok);
    Smb_EndTurn();
    if (ok && out) *out = (BYTE)(v & 0xFF);
    return ok;
}

int Smb_Write8(unsigned char addr8, unsigned char reg, unsigned char val) {
    int ok;
    if (!s_inited) Smb_Init();
    if (BootSettling()) return 0;         /* hold off the bus until it has settled */
    Smb_BeginTurn("SMB");
    TRACEU("smb.wr>", "addr/reg", ((unsigned long)addr8 << 8) | reg);
    ok = (HalWriteSMBusValue(addr8, reg, FALSE, (DWORD)val) == 0);
    TRACEU("smb.wr<", "ok", (unsigned long)ok);
    NoteResult(ok);
    Smb_EndTurn();
    return ok;
}

void Smb_EmergencyReset(void) {
    /* Intentionally a no-op. We used to W1C the nForce status port here to
       "unstick" a wedged kernel SMBus call -- but that direct poke is precisely
       what corrupts an in-flight kernel transaction and CAUSES the wedge. The
       controller belongs to the kernel, and a raised-IRQL spin can't be broken
       from another thread anyway. The watchdog still logs the event (its real
       value); it no longer touches the controller. Kept so the call links. */
}

void Smb_SetTurnGapUs(unsigned us) { s_turnGapUs = us; }
void Smb_SetFailReset(int n) { if (n >= 1) s_failReset = n; }
void Smb_SetBootSettleMs(unsigned ms) { s_bootSettleMs = ms; }
int  Smb_Ready(void) { return (s_inited && !BootSettling()) ? 1 : 0; }