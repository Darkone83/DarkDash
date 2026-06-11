#ifndef DD_SMBSVC_H
#define DD_SMBSVC_H
/*---------------------------------------------------------------------------
    dd_smbsvc.h -- the SMBus service thread: the SOLE owner of the bus.

    Architecture (modeled on XBMC4Gamers' threaded LCD/fan and the Type-D's
    single-master discipline): one low-priority background thread does ALL
    steady-state SMBus work -- sensor reads (temps/fan), the LCD panel writes,
    and the one-time revision probe -- on a slow round-robin. The main/render
    thread never touches the bus; it only reads values the service thread has
    published (Sys_ReadTemps/Sys_ReadFanPct are now cache-only) and sets LCD
    line content. Everything funnels through the dd_smbus broker (preflight,
    boot-settle gate, inter-turn guard), so the broker is only ever entered by
    this one thread.

    Why: a slow, serial, blocking peripheral has no business on a 60fps render
    loop. With it isolated here, a soft bus stall freezes only this worker --
    the UI and audio keep running, and the main-thread watchdog stops
    mis-reading bus stalls as render hangs. (A true high-IRQL kernel spin still
    takes the whole CPU regardless of thread; nothing in software escapes that.
    This design's job is to make that case far less likely and every lesser
    case a non-event.)

    Start after Sys_Init + Lcd_Init + Watchdog_Init. The broker's boot-settle
    gate holds this thread's early transactions until the bus is safe, so it's
    fine to start it immediately.
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

    void Smbsvc_Init(void);   /* start the service thread once (idempotent) */
    void Smbsvc_Stop(void);   /* signal + join (bounded); call before audio/gfx teardown */

#ifdef __cplusplus
}
#endif
#endif /* DD_SMBSVC_H */