#ifndef DARKDASH_ISO_H
#define DARKDASH_ISO_H
/*---------------------------------------------------------------------------
    dd_iso -- orthographic isometric camera for DarkDash chrome/stage.

    The camera supplies the tilt: panels authored in the virtual 640x480 grid
    are placed as quads on a world plane and rendered through an ortho
    projection rotated to the iso angle. Flat frame art therefore appears
    tilted without re-authoring.

    Content text is NOT drawn through this camera. Instead, Iso_Project maps a
    panel's anchor to where it lands on screen, and the caller draws the text
    flat (axis-aligned, readable) at that projected position.

    Angles are tunable at runtime so the tilt can be dialled in on hardware.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>
#include "dd_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

    void Iso_SetAngles(float pitchDeg, float yawDeg);
    void Iso_SetBreathe(float dPitch, float dYaw);   /* transient idle "breathe" offset */
    void Iso_GetAngles(float* pitchDeg, float* yawDeg);
    void Iso_NudgeAngles(float dPitch, float dYaw);

    /* Set the iso transforms on the device. Call before Iso_DrawPanel calls. */
    void Iso_Begin(void);
    void Iso_End(void);

    /* Draw a panel authored in virtual coords as a quad on the tilted plane.
       colour modulates (0xFFFFFFFF = untinted); additive!=0 for glow overlays. */
    void Iso_DrawPanel(const Texture* t, float vx, float vy, float vw, float vh,
        DWORD colour, int additive);

    /* Solid (untextured) quad on the tilted plane -- used for the selection
       highlight so it tilts with the menu. additive!=0 for a glow fill. */
    void Iso_FillRect(float vx, float vy, float vw, float vh,
        DWORD colour, int additive);

    /* Project a virtual-layout anchor through the iso transform.
       Returns the VIRTUAL screen coords where it lands (feed straight to
       Font_DrawText / UI_* which scale virtual->backbuffer). */
    void Iso_Project(float vx, float vy, float* outVx, float* outVy);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_ISO_H */