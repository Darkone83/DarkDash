/*---------------------------------------------------------------------------
    dd_update.h -- inline OTA self-updater for DarkDash.

    Ported from XbDiag Update.cpp: a non-blocking HTTP state machine that
    checks a remote version, and on demand downloads + extracts an update
    archive over the running install, then relaunches.

    Server (HTTP):
        host   darkone83.myddns.me : 8008
        ver    /darkdash/DarkDash.ver        (e.g. "1.0.0")
        archive/darkdash/update.xba
        log    /darkdash/log.chg             (optional changelog)
    Local:
        archive temp  D:\update.xba
        extract dir   the running XBE's dir (D:\)
        relaunch      D:\default.xbe

    This module owns the networking, download, XBA extraction and relaunch.
    The Settings Update panel drives it (Upd_StartCheck / Upd_StartDownload /
    Upd_Tick) and renders from the status getters -- the core does no drawing.
---------------------------------------------------------------------------*/
#ifndef DD_UPDATE_H
#define DD_UPDATE_H

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    enum {
        UPD_IDLE = 0,      /* nothing started yet            */
        UPD_CHECKING,      /* net init / DNS / fetching .ver */
        UPD_UPTODATE,      /* checked: local >= remote       */
        UPD_AVAILABLE,     /* checked: remote is newer       */
        UPD_DOWNLOADING,   /* downloading update.xba         */
        UPD_EXTRACTING,    /* extracting update.xba          */
        UPD_DONE,          /* update written; ready to relaunch */
        UPD_ERROR          /* see Upd_Error()                */
    };

    void Upd_Init(const char* localVersion);   /* pass DARKDASH_VERSION */
    void Upd_StartCheck(void);                  /* begin a version check  */
    void Upd_StartDownload(void);              /* begin download+extract (from AVAILABLE/UPTODATE) */
    void Upd_Tick(void);                        /* advance the state machine (call each frame) */
    void Upd_Cancel(void);                      /* abort + return to IDLE  */
    void Upd_Relaunch(void);                    /* XLaunchNewImage the new XBE (after UPD_DONE) */

    /* Register a render callback invoked periodically during the blocking download
       so the progress bar advances on screen (XbDiag pattern). Pass NULL to clear.
       The callback should draw one full frame (begin/draw/end). */
    typedef void (*UpdRenderFn)(void);
    void Upd_SetRenderFn(UpdRenderFn fn);

    int  Upd_State(void);                       /* one of UPD_*            */
    const char* Upd_LocalVersion(void);
    const char* Upd_RemoteVersion(void);        /* valid after a check     */
    const char* Upd_Error(void);                /* message when UPD_ERROR  */
    int  Upd_Progress(void);                    /* 0..100 during DL/extract*/

    /* changelog (optional): fetched lazily; lines for the panel to scroll */
    const char* Upd_Changelog(void);            /* NUL-terminated text, may be "" */
    int  Upd_ChangelogReady(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_UPDATE_H */