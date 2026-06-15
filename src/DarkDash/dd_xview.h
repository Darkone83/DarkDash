#ifndef DD_XVIEW_H
#define DD_XVIEW_H
/*---------------------------------------------------------------------------
    dd_xview.h -- DarkDash integration for the X-View USB color panel.

    X-View is the LCD / Type-D experience cranked up: a 320x240 ST7789 panel
    over USB with full framebuffer control, color, blit, and animation. All of
    it runs on a dedicated SERVICE THREAD (like the SMBus service thread) so the
    slow USB blits never stall the 60fps TV UI -- the panel animates on its own.

    The thread owns ALL X-View USB I/O (xv_xbox / xv_client). Nothing else
    touches the device. The only cross-thread data is a small theme snapshot,
    written from the main thread and read by the panel thread (plain ints; a torn
    read just tints one frame slightly off, which is invisible).

    Persistence is a self-contained D:\data\xview.dat (enable + brightness), NOT
    settings.dat -- adding a field there would reset everyone's saved settings.
---------------------------------------------------------------------------*/
#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* --- config (xview.dat) --- */
    int  XView_IsEnabled(void);
    void XView_SetEnabled(int on);   /* persists, then starts/stops the panel thread */
    int  XView_Brightness(void);     /* 0..255 */
    void XView_SetBrightness(int duty0to255);

    /* --- page selection + rotation (X-View's OWN config; works with no LCD) --- */
    int  XView_Pages(void);              /* LCD_PAGE_* bitmask */
    void XView_SetPages(int mask);
    void XView_TogglePage(int bit);
    int  XView_IntervalMs(void);         /* page rotation interval, ms */
    void XView_SetIntervalMs(int ms);
    int  XView_Panel(void);              /* XV_PANEL_A / XV_PANEL_B */
    void XView_SetPanel(int panel);

    /* --- lifecycle (panel thread owns warm-up, connect, render, shutdown) --- */
    void XView_SetSaverCountdown(int ms); /* ms until saver fires (0=active, <0=n/a) */
    void XView_Start(void);          /* spawn the thread (no-op if disabled/running) */
    void XView_Stop(void);           /* outro + USB shutdown + join */
    void XView_NowPlayingLaunch(const char* title, const char* xbePath);
    /* freeze a title+art frame, then shut down,
       leaving it on the panel while a game runs */
    int  XView_IsReady(void);        /* panel connected and drawing */

    /* --- theming --- */
    void XView_RefreshTheme(void);   /* snapshot DarkDash theme colors; call at start
                                        and whenever the theme changes (main thread) */

#ifdef __cplusplus
}
#endif
#endif /* DD_XVIEW_H */