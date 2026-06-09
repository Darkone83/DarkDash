/*---------------------------------------------------------------------------
    dd_arcs.h -- "frame arcs" main-menu chrome effect.

    Occasional Jacob's-ladder energy arcs that crackle along the menu frame's
    border, bowing only slightly outward (tight to the frame) and drawn BEHIND
    the frame panel so their roots tuck behind the border. Purely cosmetic,
    self-contained, main-menu-only. Gated by the DD_FX_ARCS effect toggle at the
    call site.

    Usage (inside the main menu's Iso_Begin()/Iso_End(), BEFORE the frame panel):
        Arcs_Tick(GetTickCount());
        Arcs_Draw(menuX, menuY, frameW, frameH);
---------------------------------------------------------------------------*/
#ifndef DD_ARCS_H
#define DD_ARCS_H

#ifdef __cplusplus
extern "C" {
#endif

    /* Advance/expire/spawn arcs. Cheap; call once per frame while the effect is
       enabled and the menu is at rest. */
    void Arcs_Tick(unsigned long now);

    /* Draw the live arcs hugging the given frame rect (virtual coords). Must be
       called between Iso_Begin() and Iso_End(), before the frame panel, so the
       frame occludes the arc roots. */
    void Arcs_Draw(float fx, float fy, float fw, float fh);

#ifdef __cplusplus
}
#endif

#endif /* DD_ARCS_H */