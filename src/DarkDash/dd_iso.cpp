/*---------------------------------------------------------------------------
    dd_iso.cpp -- orthographic isometric camera + tilted panel draw.
    Uses D3DX8 for the matrix math (links d3dx8.lib).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>
#include <d3dx8.h>
#include "dd_iso.h"
#include "dd_gfx.h"
#include "dd_ui.h"   /* UI_VIRT_W / UI_VIRT_H */
#include "dd_theme.h"
#include "dd_data.h"

/* Set to 0 for a definitive no-shadow comparison on Xbox hardware.
   This switch affects only the isometric panel's cast shadow. */
#ifndef DD_ISO_PANEL_SHADOW_ENABLED
#define DD_ISO_PANEL_SHADOW_ENABLED 1
#endif

   /* Only alpha-masked light is allowed on decorative isometric panels.
      Switch to 0 to compare the raw authored frame without any UI lighting. */
#ifndef DD_ISO_FRAME_LIGHTING_ENABLED
#define DD_ISO_FRAME_LIGHTING_ENABLED 1
#endif

#define ISO_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct { float x, y, z; DWORD colour; float u, v; } IsoVert;

/* Starting tilt -- dial these in on hardware via the live nudge. */
static float s_pitch = 26.0f;   /* tilt the top of the plane away from viewer */
static float s_yaw = -16.0f;  /* recede the other way, matching the concept  */

/* transient ambient "breathe" added on top of the tuned angles (idle life).
   Kept separate so it never mutates the persistent tuned base. */
static float s_breatheP = 0.0f;
static float s_breatheY = 0.0f;

static D3DXMATRIX s_world, s_view, s_proj, s_combined;

static void iso_build(void) {
    D3DXMATRIX rx, ry, tmp;
    D3DXVECTOR3 eye, at, up;

    /* world: rotate the (centred) layout plane: yaw then pitch (+ breathe) */
    D3DXMatrixRotationX(&rx, D3DXToRadian(s_pitch + s_breatheP));
    D3DXMatrixRotationY(&ry, D3DXToRadian(s_yaw + s_breatheY));
    D3DXMatrixMultiply(&s_world, &ry, &rx);

    /* view: straight-on ortho camera looking down +Z (left-handed) */
    eye.x = 0.0f; eye.y = 0.0f; eye.z = -800.0f;
    at.x = 0.0f; at.y = 0.0f; at.z = 0.0f;
    up.x = 0.0f; up.y = 1.0f; up.z = 0.0f;
    D3DXMatrixLookAtLH(&s_view, &eye, &at, &up);

    /* proj: ortho a little larger than the layout so the tilt has room */
    D3DXMatrixOrthoLH(&s_proj, UI_VIRT_W * 1.30f, UI_VIRT_H * 1.30f,
        -2000.0f, 2000.0f);

    /* combined (world*view*proj) for manual anchor projection */
    D3DXMatrixMultiply(&tmp, &s_world, &s_view);
    D3DXMatrixMultiply(&s_combined, &tmp, &s_proj);
}

void Iso_SetAngles(float pitchDeg, float yawDeg) { s_pitch = pitchDeg; s_yaw = yawDeg; }
void Iso_SetBreathe(float dPitch, float dYaw) { s_breatheP = dPitch; s_breatheY = dYaw; }
void Iso_GetAngles(float* p, float* y) { if (p) *p = s_pitch; if (y) *y = s_yaw; }
void Iso_NudgeAngles(float dP, float dY) {
    s_pitch += dP; s_yaw += dY;
    if (s_pitch < 0.0f)  s_pitch = 0.0f;
    if (s_pitch > 70.0f) s_pitch = 70.0f;
    if (s_yaw < -45.0f) s_yaw = -45.0f;
    if (s_yaw > 45.0f) s_yaw = 45.0f;
}

void Iso_Begin(void) {
    IDirect3DDevice8* d = Gfx_Device();
    if (!d) return;
    iso_build();
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&s_world);
    d->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&s_view);
    d->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&s_proj);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
}

void Iso_End(void) {
    IDirect3DDevice8* d = Gfx_Device();
    if (!d) return;
    /* XYZRHW (text/flat overlay) ignores transforms, so no reset needed;
       just leave a sane z state for the 2D pass. */
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

/* Lighting must respect the frame texture alpha. Previous UI lighting
   used untextured bands and edge strips that illuminated the entire panel
   bounding rectangle, including the transparent corners of frame_menu_v.
   That produced the straight purple/blue edge beside the decorative frame.

   This draws one subtle, theme-colored light gradient over the actual frame
   silhouette. Texture alpha gates the light at every pixel; the light has
   no hard horizontal or vertical bounds of its own. */
#if DD_A4_DEPTH && DD_A5_LIGHTING && DD_ISO_FRAME_LIGHTING_ENABLED
static void iso_masked_frame_light(const Texture* t, float vx, float vy,
    float vw, float vh, DWORD rgb, int topAlpha) {
    IDirect3DDevice8* d = Gfx_Device();
    IsoVert v[4];
    float cx = UI_VIRT_W * 0.5f, cy = UI_VIRT_H * 0.5f;
    float x0, x1, y0, y1, u1, v1;
    if (!d || !t || !t->tex || topAlpha <= 0 || vw <= 0.0f || vh <= 0.0f) return;
    if (topAlpha > 72) topAlpha = 72;
    x0 = vx - cx; x1 = vx + vw - cx;
    y0 = cy - vy; y1 = cy - (vy + vh);
    u1 = (float)t->w / (float)t->pw;
    v1 = (float)t->h / (float)t->ph;
    v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].colour = (((DWORD)topAlpha) << 24) | (rgb & 0x00FFFFFF); v[0].u = 0; v[0].v = 0;
    v[1].x = x1; v[1].y = y0; v[1].z = 0; v[1].colour = (((DWORD)(topAlpha * 2 / 3)) << 24) | (rgb & 0x00FFFFFF); v[1].u = u1; v[1].v = 0;
    v[2].x = x0; v[2].y = y1; v[2].z = 0; v[2].colour = rgb & 0x00FFFFFF; v[2].u = 0; v[2].v = v1;
    v[3].x = x1; v[3].y = y1; v[3].z = 0; v[3].colour = rgb & 0x00FFFFFF; v[3].u = u1; v[3].v = v1;
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTexture(0, t->tex);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    d->SetVertexShader(ISO_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(IsoVert));
    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}
#endif

/* Batched soft cast shadow, masked by the *same texture alpha* as the panel.
   A weighted set of slightly displaced silhouettes approximates a smooth
   penumbra. Unlike a panel-sized filled rectangle, transparent cutouts
   remain transparent. The fourteen samples are submitted in one draw call;
   each triangle still uses the source art alpha for blending. */
#if DD_A4_DEPTH && DD_ISO_PANEL_SHADOW_ENABLED
struct IsoShadowTap { float dx, dy; int weight; };
#define ISO_SHADOW_TAPS 14
static const IsoShadowTap s_shadowTaps[ISO_SHADOW_TAPS] = {
    /* Broad, low-opacity penumbra around the offset silhouette. */
    { 0.0f,  0.0f, 28},
    {-3.5f,  0.0f, 12}, { 3.5f,  0.0f, 12},
    { 0.0f, -3.5f, 12}, { 0.0f,  3.5f, 12},
    {-3.5f, -3.5f, 7}, { 3.5f, -3.5f, 7},
    {-3.5f,  3.5f, 7}, { 3.5f,  3.5f, 7},
    {-7.0f,  0.0f, 3}, { 7.0f,  0.0f, 3},
    { 0.0f, -7.0f, 3}, { 0.0f,  7.0f, 3},
    /* Slightly closer contact shadow anchors the frame to its backdrop. */
    {-2.0f, -2.0f, 22}
};

static void iso_masked_shadow(const Texture* t, float vx, float vy,
    float vw, float vh, int alpha) {
    IDirect3DDevice8* d = Gfx_Device();
    IsoVert verts[ISO_SHADOW_TAPS * 6];
    float cx = UI_VIRT_W * 0.5f, cy = UI_VIRT_H * 0.5f;
    float uMax, vMax;
    int i, j, count = 0;
    static const int index[6] = { 0,1,2,1,3,2 };
    if (!d || !t || !t->tex || vw <= 0.0f || vh <= 0.0f ||
        t->pw <= 0 || t->ph <= 0 || alpha <= 0) return;
    if (alpha > 255) alpha = 255;
    uMax = (float)t->w / (float)t->pw;
    vMax = (float)t->h / (float)t->ph;
    for (i = 0; i < ISO_SHADOW_TAPS; ++i) {
        float x0, x1, y0, y1;
        float px[4], py[4], tu[4], tv[4];
        DWORD black;
        int a = alpha * s_shadowTaps[i].weight / 100;
        if (a <= 0) continue;
        x0 = vx + 4.5f + s_shadowTaps[i].dx - cx;
        x1 = x0 + vw;
        y0 = cy - (vy + 6.5f + s_shadowTaps[i].dy);
        y1 = y0 - vh;
        px[0] = x0; py[0] = y0; tu[0] = 0.0f; tv[0] = 0.0f;
        px[1] = x1; py[1] = y0; tu[1] = uMax; tv[1] = 0.0f;
        px[2] = x0; py[2] = y1; tu[2] = 0.0f; tv[2] = vMax;
        px[3] = x1; py[3] = y1; tu[3] = uMax; tv[3] = vMax;
        black = ((DWORD)a) << 24;
        for (j = 0; j < 6; ++j) {
            int k = index[j];
            verts[count].x = px[k]; verts[count].y = py[k];
            verts[count].z = 0.0f;
            verts[count].colour = black;
            verts[count].u = tu[k]; verts[count].v = tv[k];
            ++count;
        }
    }
    if (!count) return;
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTexture(0, t->tex);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    d->SetVertexShader(ISO_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, count / 3, verts, sizeof(IsoVert));
}
#endif

void Iso_DrawPanel(const Texture* t, float vx, float vy, float vw, float vh,
    DWORD colour, int additive) {
    IDirect3DDevice8* d = Gfx_Device();
    IsoVert v[4];
    float cx = UI_VIRT_W * 0.5f, cy = UI_VIRT_H * 0.5f;
    float x0, x1, y0, y1, u1, v1;

    if (!d || !t || !t->tex) return;
#if DD_A6_INTERACTION && DD_A4_DEPTH && DD_A5_LIGHTING
    int arrival = additive ? 0 : UI_PanelArrivalAlpha(vx, vy, vw, vh, 1);
#else
    int arrival = 0;
    (void)arrival;
#endif
#if DD_A4_DEPTH && DD_ISO_PANEL_SHADOW_ENABLED
    if (!additive) {
        /* Shadow is masked by this actual panel texture, so irregular
           transparent artwork cannot reveal a rectangular bounding quad. */
        int a = (int)((colour >> 24) & 255);
        float settle = (float)arrival * (2.0f / 22.0f);
#if DD_VISUAL_IMPACT
        int alpha = 84 * a / 255;
#else
        int alpha = 62 * a / 255;
#endif
        iso_masked_shadow(t, vx + settle, vy + settle, vw, vh,
            Data_EffectScale(DD_EFFECT_SHADOW, alpha));
    }
#endif

    /* virtual -> centred world XY (flip Y so up is +Y), z = 0 */
    x0 = vx - cx;          x1 = vx + vw - cx;
    y0 = cy - vy;          y1 = cy - (vy + vh);
    u1 = (float)t->w / (float)t->pw;
    v1 = (float)t->h / (float)t->ph;

    v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].colour = colour; v[0].u = 0;  v[0].v = 0;
    v[1].x = x1; v[1].y = y0; v[1].z = 0; v[1].colour = colour; v[1].u = u1; v[1].v = 0;
    v[2].x = x0; v[2].y = y1; v[2].z = 0; v[2].colour = colour; v[2].u = 0;  v[2].v = v1;
    v[3].x = x1; v[3].y = y1; v[3].z = 0; v[3].colour = colour; v[3].u = u1; v[3].v = v1;

    d->SetTexture(0, t->tex);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);

    d->SetVertexShader(ISO_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(IsoVert));

    d->SetTexture(0, NULL);
#if DD_A4_DEPTH && DD_A5_LIGHTING && DD_ISO_FRAME_LIGHTING_ENABLED
    if (!additive && vw >= 20.0f && vh >= 20.0f) {
        const ThemeStageLighting* lighting = Theme_StageLighting();
        int a = (int)((colour >> 24) & 255);
        if (lighting && a > 0) {
            DWORD accent = Theme_Color("accent", 0xFF7FE000);
            DWORD key = lighting->key;
            int r = ((((accent >> 16) & 255) * 2) + ((key >> 16) & 255)) / 3;
            int g = ((((accent >> 8) & 255) * 2) + ((key >> 8) & 255)) / 3;
            int b = (((accent & 255) * 2) + (key & 255)) / 3;
            int strength = lighting->keyLevel;
            int alpha;
            if (strength < 0) strength = 0;
            if (strength > 150) strength = 150;
#if DD_VISUAL_IMPACT
            alpha = lighting->material == DD_STAGE_HOLOGRAM ? 58
                : (lighting->material == DD_STAGE_METAL ? 42 : 49);
#else
            alpha = lighting->material == DD_STAGE_HOLOGRAM ? 32
                : (lighting->material == DD_STAGE_METAL ? 24 : 28);
#endif
#if DD_A6_INTERACTION
            alpha += arrival / 4;
#endif
            alpha = Data_EffectScale(DD_EFFECT_REFLECTION,
                alpha * strength * a / (100 * 255));
            iso_masked_frame_light(t, vx, vy, vw, vh,
                0xFF000000 | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b,
                alpha);
        }
    }
#endif
}

void Iso_FillRect(float vx, float vy, float vw, float vh,
    DWORD colour, int additive) {
    IDirect3DDevice8* d = Gfx_Device();
    IsoVert v[4];
    float cx = UI_VIRT_W * 0.5f, cy = UI_VIRT_H * 0.5f;
    float x0, x1, y0, y1;

    if (!d) return;
    x0 = vx - cx;  x1 = vx + vw - cx;
    y0 = cy - vy;  y1 = cy - (vy + vh);

    v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].colour = colour; v[0].u = 0; v[0].v = 0;
    v[1].x = x1; v[1].y = y0; v[1].z = 0; v[1].colour = colour; v[1].u = 0; v[1].v = 0;
    v[2].x = x0; v[2].y = y1; v[2].z = 0; v[2].colour = colour; v[2].u = 0; v[2].v = 0;
    v[3].x = x1; v[3].y = y1; v[3].z = 0; v[3].colour = colour; v[3].u = 0; v[3].v = 0;

    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(ISO_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(IsoVert));
}

#define ISO_STRIP_MAX 96

void Iso_DrawStrip(const IsoStripPt* pts, int n, int additive) {
    IDirect3DDevice8* d = Gfx_Device();
    static IsoVert v[ISO_STRIP_MAX];
    float cx = UI_VIRT_W * 0.5f, cy = UI_VIRT_H * 0.5f;
    int i;

    if (!d || !pts || n < 3) return;
    if (n > ISO_STRIP_MAX) n = ISO_STRIP_MAX;

    for (i = 0; i < n; i++) {
        v[i].x = pts[i].vx - cx;
        v[i].y = cy - pts[i].vy;       /* flip Y so up is +Y (matches panels) */
        v[i].z = 0.0f;
        v[i].colour = pts[i].colour;
        v[i].u = 0.0f; v[i].v = 0.0f;
    }

    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(ISO_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, n - 2, v, sizeof(IsoVert));
}

void Iso_Project(float vx, float vy, float* outVx, float* outVy) {
    float cx = UI_VIRT_W * 0.5f, cy = UI_VIRT_H * 0.5f;
    D3DXVECTOR3 in, out;
    in.x = vx - cx; in.y = cy - vy; in.z = 0.0f;
    D3DXVec3TransformCoord(&out, &in, &s_combined);   /* -> NDC (ortho: w=1) */
    if (outVx) *outVx = (out.x * 0.5f + 0.5f) * UI_VIRT_W;
    if (outVy) *outVy = (1.0f - (out.y * 0.5f + 0.5f)) * UI_VIRT_H;
}