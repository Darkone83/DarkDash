#ifndef DARKDASH_UI_H
#define DARKDASH_UI_H
/*---------------------------------------------------------------------------
    dd_ui -- DarkDash UI layer (replaces SceneChat ui.h).

    Owns the virtual coordinate space. All layout is authored in a virtual
    640x480 grid (matches theme.ini base_resolution) and scaled to the live
    backbuffer here, so the renderer is resolution-independent.

    UI_Sx/UI_Sy are exposed under those names so the reworked font module can
    use them directly. Also provides the flat sprite + fill primitives used by
    the chrome and content layers.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>
#include "dd_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_VIRT_W 640.0f
#define UI_VIRT_H 480.0f

    /* ARGB pack helper */
#define UI_ARGB(a,r,g,b) (((DWORD)(a)<<24)|((DWORD)(r)<<16)|((DWORD)(g)<<8)|(DWORD)(b))

/* Call once after Gfx_Init with the real backbuffer size. */
    void  UI_Init(int backW, int backH);

    /* virtual -> screen pixel scaling */
    float UI_Sx(float x);
    float UI_Sy(float y);
    float UI_ScaleX(float d);   /* scale a width/advance delta (no offset) */
    float UI_ScaleY(float d);   /* scale a height delta (no offset)        */

    /* Aspect mode: 0 = pillarbox (uniform, centered, 4:3 preserved),
       1 = stretch (fill the backbuffer, distorts on 16:9). Recomputes live. */
    void  UI_SetStretch(int stretch);

    /* Draw a texture as a flat screen-aligned quad at virtual rect (vx,vy,vw,vh).
       'colour' modulates (use 0xFFFFFFFF for untinted). additive!=0 uses ONE blend
       for glow overlays; otherwise standard src-alpha. */
    void  UI_DrawSprite(const Texture* t, float vx, float vy, float vw, float vh,
        DWORD colour, int additive);

    /* Draw a texture at its native size (scaled by the virtual->screen factor). */
    void  UI_DrawSpriteNative(const Texture* t, float vx, float vy,
        DWORD colour, int additive);

    /* Solid colour rectangle in virtual coords. */
    void  UI_FillRect(float vx, float vy, float vw, float vh, DWORD colour);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_UI_H */