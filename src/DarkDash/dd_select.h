/*---------------------------------------------------------------------------
    dd_select.h -- shared "alive" selection highlight.

    A single animated highlight used by every menu (main, settings console,
    launcher, file manager). Instead of the glow snapping to the selected row,
    it eases toward it with a little overshoot (spring), pops in scale when the
    row changes, and ticks a brief chromatic split -- the cyberpunk/edgy feel.

    Usage per frame, where the screen would have drawn its selection rect:
        Select_Begin(ctxId, targetY);     // ctxId distinguishes screens; a
                                          // change snaps (no slide between
                                          // unrelated menus)
        Select_DrawGlow(x, y, w, h, glowColor);   // draws the animated rect

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

    /* draw the animated highlight rect (eased Y, scale-pop, chromatic tick) on the
       iso plane, using baseColor (ARGB) as the glow. x/y/w/h are the rect at rest;
       y is overridden by the eased position from Select_Begin. */
    void Select_DrawGlow(float x, float y, float w, float h, DWORD baseColor);

    /* current eased Y (for callers that position text/extras relative to it) */
    float Select_Y(void);

    /* force the animation to snap to the next target (use on screen enter) */
    void Select_Reset(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_SELECT_H */