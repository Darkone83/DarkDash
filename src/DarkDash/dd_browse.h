/*---------------------------------------------------------------------------
    dd_browse.h -- lightweight single-pane folder picker overlay.

    Lets the user browse the filesystem and pick a folder, instead of typing a
    path on the OSK. Used by the launcher to add a custom scan path. Overlay
    model like dd_osk: open it, pump Browse_Update() each frame, draw it after
    the underlying screen with Browse_Draw().

    Controls:
        Up / Down    move cursor
        A            enter the highlighted folder (or drive)
        B            up one level / cancel at the drive list
        Y or START   SELECT the current folder  -> confirm
        Back         cancel

    Browse_Update returns: 0 still open, 1 confirmed (path in buffer), -1 cancelled.
---------------------------------------------------------------------------*/
#ifndef DD_BROWSE_H
#define DD_BROWSE_H

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    void Browse_Open(const char* title);   /* open at the drive list */
    void Browse_Close(void);
    int  Browse_IsOpen(void);
    int  Browse_Update(WORD pressed);      /* 0 open, 1 confirm, -1 cancel */
    void Browse_Draw(IDirect3DDevice8* d);
    void Browse_GetPath(char* buf, int buflen);   /* chosen folder after confirm */

#ifdef __cplusplus
}
#endif
#endif /* DD_BROWSE_H */