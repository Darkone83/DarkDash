/*---------------------------------------------------------------------------
    dd_select.h -- shared "alive" selection highlight.

    A single translucent highlight used by every menu (main, settings,
    launcher, file manager, save manager). A theme-coloured alpha-blended fill
    and thin outline move with a restrained spring/pop, without opaque artwork
    or additive blending that can wash out the menu text.

    Usage per frame, where the screen would have drawn its selection rect:
        Select_Begin(ctxId, targetY);     // ctxId distinguishes screens; a
                                          // change snaps (no slide between
                                          // unrelated menus)
        Select_DrawGlow(x, y, w, h, themeAccent); // alpha-blended highlight

    Call Select_Reset() when entering a screen to avoid a slide-in from the
    previous menu's position.
---------------------------------------------------------------------------*/
#ifndef DD_SELECT_H
#define DD_SELECT_H

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* advance the animation toward targetY for the given context. ctxId is any
       stable per-screen tag; when it changes, the highlight snaps instead of
       sliding across from an unrelated layout. Call once per frame before drawing. */
    void Select_Begin(int ctxId, float targetY);

    /* Draw the shared translucent row highlight on the isometric plane.
       x/y/w/h describe the rest rectangle; Y follows Select_Begin's spring.
       baseColor supplies the theme RGB and optional fade intensity. */
    void Select_DrawGlow(float x, float y, float w, float h, DWORD baseColor);

    /* Flat-screen variant for File Manager / Save Manager. Uses the same
       spring/pop timing and alpha treatment through UI_FillRect. */
    void Select_DrawGlowFlat(float x, float y, float w, float h, DWORD baseColor);

    /* current eased Y (for callers that position text/extras relative to it) */
    float Select_Y(void);

    /* Non-animated highlight for modal/popup rows; the same fill and outline
       as the normal selection, but does not touch the spring animation. */
    void Select_DrawBoxFlat(float x, float y, float w, float h, DWORD baseColor);

    /* Optional short focus response for a popup's selected row. Unlike the
       main-menu spring, this uses a separate context so popup navigation can
       never displace the underlying screen's selection. */
    void Select_BeginModal(int ctxId, int selectedIndex);
    void Select_DrawModalBoxFlat(float x, float y, float w, float h, DWORD baseColor);

    /* force the animation to snap to the next target (use on screen enter) */
    void Select_Reset(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_SELECT_H */