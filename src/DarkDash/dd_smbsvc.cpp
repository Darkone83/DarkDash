/*---------------------------------------------------------------------------
    dd_smbsvc.cpp -- see dd_smbsvc.h.

    Build: MSVC2003/C89 style; file-scope statics; no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_smbsvc.h"
#include "dd_smbus.h"      /* broker (boot-settle gate, serialization)        */
#include "dd_sysinfo.h"    /* Sys_ServiceSensors, Sys_XboxRevision            */
#include "dd_lcd.h"        /* Lcd_PollSensors, Lcd_Tick                       */
#include "dd_watchdog.h"   /* Watchdog_SvcBeat -- bus-owner liveness          */
#include "dd_trace.h"

#define SVC_CADENCE_MS   180   /* loop period; the sensor (1s) and LCD (1Hz/4Hz)
                                  cadences are enforced inside their own calls,
                                  so this just has to spin often enough to feed
                                  them on time without busy-waiting. */
#define SVC_STOP_WAIT_MS 2000  /* bounded join on shutdown                      */

static HANDLE s_thread = NULL;
static LONG   s_stop = 0;
static int    s_revDone = 0;   /* system version read once when the bus goes live */

static DWORD WINAPI SvcProc(LPVOID p) {
    (void)p;
    TRACE_THREAD("SVC");
    for (;;) {
        if (InterlockedCompareExchange(&s_stop, 0, 0)) break;

        /* Beat the bus-owner watchdog at the top of the loop: reaching here means
           the previous iteration's bus work completed (didn't wedge). If a kernel
           SMBus call hangs, this beat stops and the watchdog logs it. */
        Watchdog_SvcBeat();
        TRACE("svc.loop", "top");

        /* System version is immutable -- read it EXACTLY ONCE, the first time the
           bus is live, then never touch it again. */
        if (!s_revDone && Smb_Ready()) {
            TRACE("svc.rev", ">");
            Sys_XboxRevision();
            TRACE("svc.rev", "<");
            s_revDone = 1;
        }

        /* Sensor reads (temps + fan) -> published cache. */
        TRACE("svc.sens", ">");
        Sys_ServiceSensors();
        TRACE("svc.sens", "<");

        /* LCD: copy the freshly-published sensor cache into the panel's display
           fields (bus-free now), then flush the panel. Lcd_Tick self-paces to
           1Hz (raw) / 4Hz (compat) and runs the deferred boot probe once
           Smb_Ready(). Both are the only LCD bus writes in the system. */
        Lcd_PollSensors();
        Lcd_Tick();

        Sleep(SVC_CADENCE_MS);
    }
    return 0;
}

void Smbsvc_Init(void) {
    if (s_thread) return;
    InterlockedExchange(&s_stop, 0);
    s_thread = CreateThread(NULL, 0, SvcProc, NULL, 0, NULL);
    /* Below the render thread so panel/sensor work never starves the UI. The
       bus is slow and we don't need fast updates -- exactly the case for a
       low-priority owner. */
    if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
}

void Smbsvc_Stop(void) {
    if (!s_thread) return;
    InterlockedExchange(&s_stop, 1);
    /* Bounded join: the loop sleeps <=SVC_CADENCE_MS, so a healthy thread exits
       promptly. If a transaction is wedged at the driver, don't hang shutdown --
       detach and let it die when (if) the kernel call returns. */
    if (WaitForSingleObject(s_thread, SVC_STOP_WAIT_MS) == WAIT_OBJECT_0) {
        CloseHandle(s_thread);
    }
    s_thread = NULL;
}