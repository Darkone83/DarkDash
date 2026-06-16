#ifndef DD_SMBUS_H
#define DD_SMBUS_H
/*---------------------------------------------------------------------------
    dd_smbus.h -- single owner of the Xbox SMBus ("the scheduler").

    Every SMBus transaction in DarkDash -- sensor reads (dd_sysinfo), the LCD
    panel writes (dd_lcd), encoder/revision probes, fan + power pokes -- now
    goes through this one broker. Nothing calls HalReadSMBusValue /
    HalWriteSMBusValue directly any more. The point is "one master, takes
    turns": only one transaction is ever in flight, the controller is cleaned
    before each turn, and a quiet guard gap is left after each turn so a
    secondary master (Type-D Expansion, OXFP) and the kernel always get a clean
    bus. Slower than the old free-for-all on purpose -- a few calm batches/sec
    is plenty for a dashboard, and it removes the random collision wedge.

    Turn model
    ----------
      Smb_BeginTurn("LCD");        -- claim the bus (blocks until it's our turn)
        Smb_Write8(addr,reg,val);  -- as many related ops as the turn needs...
        Smb_Read8 (addr,reg,&v);
      Smb_EndTurn();               -- release + leave the inter-master guard gap

    Turns nest safely (recursive). A bare Smb_Read8 / Smb_Write8 outside any
    BeginTurn is its own one-transaction turn, so callers that only need a
    single op don't have to bracket anything. The expensive controller
    preflight + inter-master guard happen ONCE per *outer* turn, not per byte,
    so an LCD page redraw is one turn (cheap) instead of 80 guarded writes.

    Threading
    ---------
      * Normal transactions are serialized by an internal critical section, so
        the main thread's sensor read can never overlap the main thread's LCD
        write -- and a future worker thread couldn't either.
      * Smb_EmergencyReset() is the watchdog thread's escape hatch. It is
        LOCK-FREE on purpose: if the main thread is wedged inside a kernel
        SMBus call (holding the lock), the watchdog must still be able to W1C
        the controller to break that spin. Do NOT take the lock there.
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

    /* Idempotent. Creates the bus lock and clears the controller once. Call
       before any sensor/LCD access (Sys_Init() already calls this). Safe to
       call more than once. */
    void Smb_Init(void);

    /* Claim / release the bus for a related burst of transactions. owner is a
       short tag ("LCD", "SYS", ...) published to the liveness watchdog so a
       stall breadcrumb shows who held the bus. Nestable. */
    void Smb_BeginTurn(const char* owner);
    void Smb_EndTurn(void);

    /* One transaction. addr8 is the software-shifted 8-bit address (e.g. 0x20,
       0x98). Returns 1 on success, 0 on failure (and *out is zeroed on a failed
       read). If called outside a turn, runs as its own one-transaction turn. */
    int  Smb_Read8(unsigned char addr8, unsigned char reg, unsigned char* out);
    int  Smb_Write8(unsigned char addr8, unsigned char reg, unsigned char val);

    /* LOCK-FREE controller reset (W1C the nForce SMBus global status at I/O
       0xC000). For the watchdog thread to break a kernel SMBus spin while the
       main thread holds the lock. Normal code does NOT need this -- the broker
       self-heals after a run of failures inside the lock. */
    void Smb_EmergencyReset(void);

    /* Tuning knobs (optional; sane defaults baked in).
         TurnGapUs : quiet inter-master guard left after each outer turn
                     (default 1500us; relaxed from Type-D's 2500us guard --
                      see dd_smbus.cpp. Safe range ~1200..2500).
         FailReset : consecutive transaction failures before the broker W1Cs
                     the controller itself (default 4). */
    void Smb_SetTurnGapUs(unsigned us);
    void Smb_SetFailReset(int n);

    /* Boot-settle gate. For the first SMB_BOOT_SETTLE_MS after Smb_Init (i.e.
       after boot/reset), the broker issues NO kernel SMBus transactions --
       Smb_Read8/Smb_Write8 return failure without touching the bus -- so nothing
       hits a still-settling SMC / controller right after an (especially
       abnormal) reset, the worst-case bus state. The W1C controller-clear in
       Smb_Init still happens immediately; only the kernel reads/writes wait.
       Smb_Ready() reports 0 during the window, 1 once it has elapsed -- callers
       that own one-time bus init (e.g. the LCD probe) use it to defer until the
       bus is safe. Default 3000 ms; raise on a flaky console. */
    void Smb_SetBootSettleMs(unsigned ms);
    int  Smb_Ready(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_SMBUS_H */