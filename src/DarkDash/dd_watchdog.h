#ifndef DD_WATCHDOG_H
#define DD_WATCHDOG_H
/*---------------------------------------------------------------------------
    dd_watchdog.h -- liveness watchdog, split across the two threads that matter.

    The SMBus service thread (dd_smbsvc) is the sole owner of the bus, so it is
    the only thing an SMBus reset can help. It beats Watchdog_SvcBeat() each loop;
    if that stops for SVC_TIMEOUT, it is wedged in a kernel SMBus call and the
    watchdog W1Cs the controller to break the spin. This is the ONLY reset path.

    The main/render thread beats Watchdog_Beat() each frame. It no longer touches
    the bus, so a main-thread stall is NOT a bus wedge and a W1C cannot help it --
    the watchdog only logs a breadcrumb to D:\data\ddwatch.log (never resets) so a
    render/logic hang is visible after the fact. Suspend/Resume bracket known long
    main-thread blocking ops to avoid a spurious breadcrumb.

    The watchdog runs ABOVE both threads' priority so it still gets the CPU.
    Define DDWATCH_TRAP to fault instead of recovering (catch a bus wedge on a
    bugcheck screen). Default is recover-and-log.
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

    void Watchdog_Init(void);                  /* start the thread once, after Sys_Init    */
    void Watchdog_Beat(void);                  /* call every frame from the present path    */
    void Watchdog_SvcBeat(void);               /* call every loop from the SMBus service thread */
    void Watchdog_SetPhase(const char* phase); /* mark the current bus phase                */
    void Watchdog_Suspend(void);               /* bracket a known long blocking op           */
    void Watchdog_Resume(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_WATCHDOG_H */