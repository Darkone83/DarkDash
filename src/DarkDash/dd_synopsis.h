/*---------------------------------------------------------------------------
    dd_synopsis.h -- title info popup.

    Shows the metadata + box art from a title's _resources pack (XBMC4Gamers
    layout): <gameFolder>\_resources\default.xml for the text fields and
    \_resources\artwork\poster.jpg for the cover. Opened from the launcher with
    WHITE, but only when a pack is actually present (Synopsis_Available()).

    Overlay model like dd_browse: open, pump Synopsis_Update() each frame, draw
    after the launcher with Synopsis_Draw().

    Video preview (preview.mp4 / a future .xmv) is intentionally NOT handled
    here yet -- this is the image + text screen that works on hardware today.
---------------------------------------------------------------------------*/
#ifndef DD_SYNOPSIS_H
#define DD_SYNOPSIS_H

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Does the title at xbePath have a usable _resources pack (default.xml)? */
    int  Synopsis_Available(const char* xbePath);

    void Synopsis_Open(const char* xbePath);   /* read pack + load art            */
    void Synopsis_Close(void);
    int  Synopsis_IsOpen(void);

    /* Returns 0 still open, 1 closed (B/back). */
    int  Synopsis_Update(WORD pressed);
    void Synopsis_Draw(IDirect3DDevice8* d);

#ifdef __cplusplus
}
#endif
#endif /* DD_SYNOPSIS_H */