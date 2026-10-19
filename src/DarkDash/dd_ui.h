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

    /* Build-wide A4 depth switch. Override with a project-wide compiler definition
       when comparing rendering, not with a local define in one .cpp. */
#ifndef DD_A4_DEPTH
#define DD_A4_DEPTH 1
#endif

       /* A5 surface/material and environment lighting; project-wide rollback.
          Needs the shared A4 depth layer, but requires no new theme.ini fields. */
#ifndef DD_A5_LIGHTING
#define DD_A5_LIGHTING 1
#endif

          /* A6: short-lived panel-arrival lighting and focus response. Shared by all
             screens; disabled with project-wide DD_A6_INTERACTION=0. The Effects >
             Selection setting still gates all motion/interaction feedback. */
#ifndef DD_A6_INTERACTION
#define DD_A6_INTERACTION 1
#endif

             /* Visually stronger materials and atmosphere, without changing legacy themes.
                Set to 0 project-wide to A/B against the previous A6 appearance. */
#ifndef DD_VISUAL_IMPACT
#define DD_VISUAL_IMPACT 1
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

    /* overscan calibration: inset all four edges by these virtual-px margins.
       Squeezes the whole 640x480 canvas into the safe rect (all screens at once). */
    void  UI_SetCalibration(float l, float r, float t, float b);

    /* Draw a texture as a flat screen-aligned quad at virtual rect (vx,vy,vw,vh).
       'colour' modulates (use 0xFFFFFFFF for untinted). additive!=0 uses ONE blend
       for glow overlays; otherwise standard src-alpha. */
    void  UI_DrawSprite(const Texture* t, float vx, float vy, float vw, float vh,
        DWORD colour, int additive);

    /* Draw a texture at its native size (scaled by the virtual->screen factor). */
    void  UI_DrawSpriteNative(const Texture* t, float vx, float vy,
        DWORD colour, int additive);

    /* Draw scalable flat chrome without stretching its decorative corners.
       Borders are in the source texture's original pixel units; destination
       coordinates remain in virtual 640x480 units. One draw call per frame. */
    void UI_DrawNineSlice(const Texture* t, float vx, float vy, float vw, float vh,
        float left, float top, float right, float bottom,
        DWORD colour, int additive);

    /* Solid colour rectangle in virtual coords. */
    void  UI_FillRect(float vx, float vy, float vw, float vh, DWORD colour);
    void  UI_FillRectAdd(float vx, float vy, float vw, float vh, DWORD colour);

    /* Interpolated vertex-colour quad: TL, TR, BL, BR corners.
       One draw call, no texture, useful for inexpensive atmospheric lights. */
    void UI_FillGradient(float vx, float vy, float vw, float vh,
        DWORD topLeft, DWORD topRight, DWORD bottomLeft, DWORD bottomRight,
        int additive);

    /* Render on top of a flat panel's artwork/backing but BEFORE text.
       Same theme/material treatment as Iso_DrawPanel; respects modal fade. */
    void UI_DrawPanelLighting(float vx, float vy, float vw, float vh, int opacity);

    /* Optional arrival highlight for existing flat/isometric panels.
       Tracks up to 32 visible rects without heap allocations; the light fades
       out after first appearance or when a panel returns after being hidden.
       kind=0 for a flat panel, 1 for an isometric panel. Return 0 when disabled. */
    int UI_PanelArrivalAlpha(float vx, float vy, float vw, float vh, int kind);

    /* Shared background elevation for all flat panels and modal dialogs.
       Must draw BEFORE the existing themed panel artwork and live text. */
    void  UI_DrawPanelDepth(float vx, float vy, float vw, float vh, int opacity);

    /* Source-alpha-aware elevation and illumination for framed flat UI.
       Zero slice margins draw the native sprite shape; nonzero margins use
       the exact same nine-slice UV mapping as the visible frame.
       Draw DepthMasked BEFORE the authored frame, LightingMasked AFTER it. */
    void UI_DrawPanelDepthMasked(const Texture* frame, float vx, float vy,
        float vw, float vh, int opacity, float left, float top, float right, float bottom);
    void UI_DrawPanelLightingMasked(const Texture* frame, float vx, float vy,
        float vw, float vh, int opacity, float left, float top, float right, float bottom);

    /* solid triangle from three virtual-space points (corner markers, etc.) */
    void  UI_FillTri(float ax, float ay, float bx, float by, float cx, float cy,
        DWORD colour);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_UI_H */