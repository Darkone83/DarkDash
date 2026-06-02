#pragma once
/*---------------------------------------------------------------------------
    dd_savemgr.h -- savegame manager (Operations view).

    A self-contained two-level browser over E:\UDATA:
        level 0: games   (TitleID folders; friendly name from TitleMeta.xbx)
        level 1: saves   (per-save folders; friendly name from SaveMeta.xbx)

    Per-save actions: Copy / Move / Delete. Copy/Move open a lightweight
    single-pane destination picker (any drive/folder, incl. memory units), then
    call the shared Fileops_* routines for the actual work. Delete confirms,
    then Fileops_Delete.

    Title art (TitleImage.xbx) is XPR-wrapped/swizzled; loading it is a planned
    follow-up. For now the view is name-driven.

    Lifecycle mirrors FileMan: SaveMgr_Enter() on open, SaveMgr_Update() each
    frame (returns 1 when the user backs all the way out -> return to main menu),
    SaveMgr_Render() to draw.
---------------------------------------------------------------------------*/
#ifndef DD_SAVEMGR_H
#define DD_SAVEMGR_H

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    void SaveMgr_Enter(void);
    int  SaveMgr_Update(WORD pressed, WORD held);   /* 1 = leave the view */
    void SaveMgr_Render(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_SAVEMGR_H */