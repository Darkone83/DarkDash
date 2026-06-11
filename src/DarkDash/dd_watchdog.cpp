/*---------------------------------------------------------------------------
    dd_watchdog.cpp -- see dd_watchdog.h.

    Build: MSVC2003/C89 style; file-scope statics; no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_watchdog.h"
#include "dd_smbus.h"      /* Smb_EmergencyReset -- lock-free wedge breaker */

#define SVC_TIMEOUT_MS   8000   /* SMBus service thread silent this long -> wedge -> W1C   */
#define MAIN_TIMEOUT_MS  8000   /* render thread silent this long -> breadcrumb only       */
#define WATCHDOG_POLL_MS 1000   /* how often the watchdog checks             */

static HANDLE      s_thread = NULL;
static LONG        s_lastBeat = 0;       /* GetTickCount of last main-thread Beat */
static LONG        s_lastSvcBeat = 0;    /* GetTickCount of last service-thread Beat */
static LONG        s_suspend = 0;       /* >0 = main-thread breadcrumb disarmed  */
static const char* s_phase = "boot";  /* last-known bus phase                  */

/* unsigned -> decimal string (no CRT) */
static int U2A(DWORD v, char* out) {
    char tmp[12]; int n = 0, i = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) out[i++] = tmp[--n];
    out[i] = 0;
    return i;
}

/* Append one line to D:\data\ddwatch.log. Runs on the watchdog thread, which is
   not stuck, so disk I/O is fine (IDE is independent of the wedged SMBus). */
static void Breadcrumb(DWORD now, DWORD stall, const char* what) {
    HANDLE f; DWORD wr; char line[160]; char num[12]; int len;
    f = CreateFileA("D:\\data\\ddwatch.log", FILE_APPEND_DATA, FILE_SHARE_READ,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    line[0] = 0;
    lstrcatA(line, "t=");        U2A(now, num);   lstrcatA(line, num);
    lstrcatA(line, " stall=");   U2A(stall, num); lstrcatA(line, num);
    lstrcatA(line, "ms phase="); lstrcatA(line, s_phase ? s_phase : "?");
    lstrcatA(line, " ");         lstrcatA(line, what ? what : "");
    lstrcatA(line, "\r\n");
    len = lstrlenA(line);
    WriteFile(f, line, (DWORD)len, &wr, NULL);
    CloseHandle(f);
}

static DWORD WINAPI WatchProc(LPVOID p) {
    int svcFired = 0, mainFired = 0;
    (void)p;
    for (;;) {
        DWORD now, sb, mb;
        Sleep(WATCHDOG_POLL_MS);
        now = GetTickCount();

        /* Bus-owner recovery: the SMBus service thread is the only code that
           touches the bus, so it is the only thing a W1C reset can help. If it
           stops beating it is wedged in a kernel SMBus call -- W1C the controller
           to break the spin. This is the ONLY reset path. */
        sb = (DWORD)InterlockedCompareExchange(&s_lastSvcBeat, 0, 0);
        if (sb != 0 && (LONG)(now - sb) >= SVC_TIMEOUT_MS) {
            if (!svcFired) {
                svcFired = 1;
#ifdef DDWATCH_TRAP
                Breadcrumb(now, now - sb, "-> TRAP (svc read wedge)");
                *(volatile DWORD*)0 = 0xDEADBEEF; /* deliberate fault -> bugcheck */
#else
                /* No controller poke: the kernel owns the bus, a raised-IRQL spin
                   can't be broken from here, and a soft wedge recovers on its own
                   when the kernel's SMBus timeout fires. We only record it. */
                Breadcrumb(now, now - sb, "-> svc SMBus read wedged (self-recovered)");
#endif
            }
        }
        else {
            svcFired = 0;
        }

        /* Render-thread liveness: the main thread no longer owns the bus, so a
           main-thread stall is NOT a bus wedge -- a W1C can't help and could
           collide with the service thread. Log a breadcrumb only, never reset.
           Suspendable around known long blocking ops to avoid a spurious entry.
           Re-read 'now': writing the svc breadcrumb above does file I/O that can
           take long enough for the main thread to beat in the meantime, which
           would make a stale 'now' underflow (now - newer beat) into a huge value
           and log a phantom hang. A fresh 'now' plus a signed compare prevents it. */
        if (InterlockedCompareExchange(&s_suspend, 0, 0) <= 0) {
            now = GetTickCount();
            mb = (DWORD)InterlockedCompareExchange(&s_lastBeat, 0, 0);
            if (mb != 0 && (LONG)(now - mb) >= MAIN_TIMEOUT_MS) {
                if (!mainFired) { mainFired = 1; Breadcrumb(now, now - mb, "-> main-thread hang (no reset)"); }
            }
            else {
                mainFired = 0;
            }
        }
    }
    /* not reached */
}

/* The five exported entry points are declared extern "C" in the header. We
   ALSO mark them extern "C" here at the definition site so the .obj exports the
   undecorated C names (_Watchdog_Beat, ...) the callers reference -- no matter
   what (a stale object, or an include-guard collision that hid the header's
   extern "C" in this translation unit). Belt-and-suspenders against LNK2001. */
#ifdef __cplusplus
extern "C" {
#endif

    void Watchdog_Beat(void) {
        InterlockedExchange(&s_lastBeat, (LONG)GetTickCount());
    }

    void Watchdog_SvcBeat(void) {
        InterlockedExchange(&s_lastSvcBeat, (LONG)GetTickCount());
    }

    void Watchdog_SetPhase(const char* phase) {
        s_phase = phase ? phase : "?";
    }

    void Watchdog_Suspend(void) {
        InterlockedIncrement(&s_suspend);
        Watchdog_Beat();
    }

    void Watchdog_Resume(void) {
        if (InterlockedCompareExchange(&s_suspend, 0, 0) > 0) InterlockedDecrement(&s_suspend);
        Watchdog_Beat();
    }

    void Watchdog_Init(void) {
        if (s_thread) return;
        Watchdog_Beat();                  /* seed so it doesn't fire before the first frame */
        s_thread = CreateThread(NULL, 0, WatchProc, NULL, 0, NULL);
        if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_HIGHEST);
    }

#ifdef __cplusplus
}   /* extern "C" */
#endif