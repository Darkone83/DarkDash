#pragma once
/*---------------------------------------------------------------------------
    dd_disc.h -- optical disc monitor for DarkDash.

    Polls the SMC tray state. When a disc is inserted and the tray closes with
    media present, it remaps Cdrom0 to a STAGING letter (S:)
    -- NOT D:, so the dashboard keeps reading its own assets from D: (themes,
    fonts, audio). If S:\default.xbe exists the disc is flagged as an Xbox game
    and its title is read from the XBE cert.

    The main menu shows a small top-right status box when a game disc is
    present, and START launches it (via Mount_LaunchXbe, which gives the
    launched game its own D: at launch time -- S: is only our staging mount).

    Disc_Poll() is cheap: it keeps mount work stateful and internally throttled.
---------------------------------------------------------------------------*/
#ifndef DD_DISC_H
#define DD_DISC_H

#include <xtl.h>
#include "dd_path.h"

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct {
        int  present;          /* a disc is in the (closed) tray            */
        int  isXboxGame;       /* S:\default.xbe exists                     */
        char title[64];        /* XBE cert title, or "Game Disc" fallback   */
        char xbePath[DD_PATH_MAX]; /* "S:\\default.xbe" when isXboxGame       */
    } DiscState;

    void  Disc_Init(void);          /* reset state                              */
    void  Disc_Poll(void);          /* call periodically; internally throttled    */
    const DiscState* Disc_Get(void);
    int   Disc_Launch(void);        /* launch the disc game (no return on success); 0 if none */

#ifdef __cplusplus
}
#endif
#endif /* DD_DISC_H */