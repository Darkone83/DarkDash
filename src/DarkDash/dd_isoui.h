#ifndef DD_ISOUI_H
#define DD_ISOUI_H
/*---------------------------------------------------------------------------
    dd_isoui.h -- "Install ISO to HDD" wizard overlay (GAMES only).

    Overlay model like dd_browse / dd_synopsis: open it, pump IsoUi_Update()
    each frame, draw after the launcher with IsoUi_Draw(). It drives dd_browse
    internally for the folder step, so the launcher must check IsoUi_IsOpen()
    BEFORE its own Browse handler (otherwise the folder confirm gets mistaken
    for an add-scan-path).

    Flow: pick a folder -> choose an .iso in it -> choose the install drive
    (only if more than one games root is mounted) -> install -> result.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    void IsoUi_Open(void);            /* start the wizard (opens the folder picker) */
    void IsoUi_Close(void);
    int  IsoUi_IsOpen(void);

    /* 0 still open, 1 closed after a SUCCESSFUL install (caller should rescan),
       -1 closed / cancelled (nothing changed). */
    int  IsoUi_Update(WORD pressed);
    void IsoUi_Draw(IDirect3DDevice8* d);

#ifdef __cplusplus
}
#endif
#endif /* DD_ISOUI_H */