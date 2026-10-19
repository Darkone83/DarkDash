/*---------------------------------------------------------------------------
    dd_ui.cpp -- virtual coordinate scaling + flat sprite/fill primitives.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_ui.h"
#include "dd_gfx.h"
#include "dd_theme.h"
#include "dd_data.h"

static int   s_backW = 640;
static int   s_backH = 480;
static int   s_stretch = 0;       /* 0 = pillarbox (uniform), 1 = stretch (fill) */
static float s_scale_x = 1.0f;
static float s_scale_y = 1.0f;
static float s_off_x = 0.0f;
static float s_off_y = 0.0f;

/* overscan calibration: inset (in virtual px) applied to all four edges. The
   full 640x480 virtual canvas is squeezed into the safe-area rect
   [calibL .. 640-calibR] x [calibT .. 480-calibB] so EVERY screen calibrates
   at once (all drawing goes through UI_Sx/UI_Sy). */
static float s_calL = 0.0f, s_calR = 0.0f, s_calT = 0.0f, s_calB = 0.0f;

#define UI_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
} UiVert;

/* recompute scale/offset from the stored backbuffer + aspect mode */
static void ui_recalc(void) {
    float sx = (float)s_backW / UI_VIRT_W;
    float sy = (float)s_backH / UI_VIRT_H;
    if (s_stretch) {
        /* fill the whole backbuffer; 4:3 art distorts on a 16:9 surface */
        s_scale_x = sx; s_scale_y = sy;
        s_off_x = 0.0f; s_off_y = 0.0f;
    }
    else {
        /* uniform scale keeps the 4:3 proportions (round orb); leftover is
           split into centering offsets -> pillarboxed on 16:9 */
        float s = (sx < sy) ? sx : sy;
        s_scale_x = s; s_scale_y = s;
        s_off_x = ((float)s_backW - UI_VIRT_W * s) * 0.5f;
        s_off_y = ((float)s_backH - UI_VIRT_H * s) * 0.5f;
    }

    /* fold in overscan calibration: squeeze the virtual canvas into the safe
       rect. New scale shrinks by the inset fraction; offset shifts inward so
       virtual (0,0) lands at the safe-rect corner. */
    {
        float availW = UI_VIRT_W - s_calL - s_calR;
        float availH = UI_VIRT_H - s_calT - s_calB;
        float fxr = (availW > 1.0f) ? availW / UI_VIRT_W : 1.0f;
        float fyr = (availH > 1.0f) ? availH / UI_VIRT_H : 1.0f;
        /* shift origin to the inset corner (in screen px), then shrink */
        s_off_x = s_off_x + s_calL * s_scale_x;
        s_off_y = s_off_y + s_calT * s_scale_y;
        s_scale_x = s_scale_x * fxr;
        s_scale_y = s_scale_y * fyr;
    }
}

void UI_SetCalibration(float l, float r, float t, float b) {
    s_calL = l; s_calR = r; s_calT = t; s_calB = b;
    ui_recalc();
}

void UI_Init(int backW, int backH) {
    s_backW = backW; s_backH = backH;
    ui_recalc();
}

void UI_SetStretch(int stretch) {
    s_stretch = stretch ? 1 : 0;
    ui_recalc();
}

float UI_Sx(float x) { return s_off_x + x * s_scale_x; }
float UI_Sy(float y) { return s_off_y + y * s_scale_y; }
float UI_ScaleX(float d) { return d * s_scale_x; }   /* width/advance delta  */
float UI_ScaleY(float d) { return d * s_scale_y; }   /* height delta         */

/* Common quad emit. tex==NULL => solid fill (colour only). */
static void ui_quad_mask(const Texture* tex,
    float vx, float vy, float vw, float vh,
    DWORD colour, int additive, int alphaMask, int falloff) {
    IDirect3DDevice8* d = Gfx_Device();
    UiVert v[4];
    float x0, y0, x1, y1, u1, v1;

    if (!d) return;

    x0 = UI_Sx(vx);        y0 = UI_Sy(vy);
    x1 = UI_Sx(vx + vw);   y1 = UI_Sy(vy + vh);

    if (tex && tex->tex) {
        u1 = (float)tex->w / (float)tex->pw;
        v1 = (float)tex->h / (float)tex->ph;
    }
    else {
        u1 = 1.0f; v1 = 1.0f;
    }

    v[0].x = x0; v[0].y = y0; v[0].z = 0; v[0].rhw = 1; v[0].colour = colour; v[0].u = 0;  v[0].v = 0;
    v[1].x = x1; v[1].y = y0; v[1].z = 0; v[1].rhw = 1; v[1].colour = colour; v[1].u = u1; v[1].v = 0;
    v[2].x = x0; v[2].y = y1; v[2].z = 0; v[2].rhw = 1; v[2].colour = colour; v[2].u = 0;  v[2].v = v1;
    v[3].x = x1; v[3].y = y1; v[3].z = 0; v[3].rhw = 1; v[3].colour = colour; v[3].u = u1; v[3].v = v1;

    if (falloff) {
        /* Interpolate light down the textured frame, not across a
           free-standing rectangle. Texture alpha gates every pixel. */
        v[2].colour &= 0x00FFFFFFu;
        v[3].colour &= 0x00FFFFFFu;
    }
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);

    if (tex && tex->tex) {
        d->SetTexture(0, tex->tex);
        d->SetTextureStageState(0, D3DTSS_COLOROP, alphaMask ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE);
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
    }
    else {
        d->SetTexture(0, NULL);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    }

    d->SetVertexShader(UI_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(UiVert));

    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
}

void UI_DrawSprite(const Texture* t, float vx, float vy, float vw, float vh,
    DWORD colour, int additive) {
    if (!t || !t->tex) return;
    ui_quad_mask(t, vx, vy, vw, vh, colour, additive, 0, 0);
}

void UI_DrawSpriteNative(const Texture* t, float vx, float vy,
    DWORD colour, int additive) {
    if (!t || !t->tex) return;
    ui_quad_mask(t, vx, vy, (float)t->w, (float)t->h, colour, additive, 0, 0);
}

/* Nine-slice flat chrome. Generate 9 cells into one triangle list rather
   than issuing nine separate DrawPrimitiveUP calls each frame. UV coordinates
   use the texture's real dimensions so power-of-two padding is never sampled.
   This is for flat frame art (not the isometric console or 3D orb). */
static void ui_nineslice(const Texture* t, float vx, float vy, float vw, float vh,
    float left, float top, float right, float bottom,
    DWORD colour, int additive, int alphaMask, int falloff) {
    IDirect3DDevice8* d = Gfx_Device();
    UiVert v[54];  /* 9 quads x 2 triangles x 3 vertices */
    float dx[4], dy[4], su[4], sv[4];
    float sx, sy;
    int col, row, count = 0;
    if (!d || !t || !t->tex || t->w <= 0 || t->h <= 0 ||
        t->pw <= 0 || t->ph <= 0 || vw <= 0.0f || vh <= 0.0f) return;

    if (left < 0.0f) left = 0.0f;
    if (right < 0.0f) right = 0.0f;
    if (top < 0.0f) top = 0.0f;
    if (bottom < 0.0f) bottom = 0.0f;
    /* Never sample overlapping source edges. */
    if (left + right > (float)t->w) {
        sx = (float)t->w / (left + right);
        left *= sx; right *= sx;
    }
    if (top + bottom > (float)t->h) {
        sy = (float)t->h / (top + bottom);
        top *= sy; bottom *= sy;
    }
    /* Keep corners within even a very small destination region. */
    if (left + right > vw) {
        sx = vw / (left + right);
        left *= sx; right *= sx;
    }
    if (top + bottom > vh) {
        sy = vh / (top + bottom);
        top *= sy; bottom *= sy;
    }
    dx[0] = UI_Sx(vx);
    dx[1] = UI_Sx(vx + left);
    dx[2] = UI_Sx(vx + vw - right);
    dx[3] = UI_Sx(vx + vw);
    dy[0] = UI_Sy(vy);
    dy[1] = UI_Sy(vy + top);
    dy[2] = UI_Sy(vy + vh - bottom);
    dy[3] = UI_Sy(vy + vh);
    su[0] = 0.0f; su[1] = left / (float)t->pw;
    su[2] = ((float)t->w - right) / (float)t->pw;
    su[3] = (float)t->w / (float)t->pw;
    sv[0] = 0.0f; sv[1] = top / (float)t->ph;
    sv[2] = ((float)t->h - bottom) / (float)t->ph;
    sv[3] = (float)t->h / (float)t->ph;

    for (row = 0; row < 3; ++row) for (col = 0; col < 3; ++col) {
        const int corners[6][2] = { {0,0},{1,0},{0,1}, {1,0},{1,1},{0,1} };
        int k;
        if (dx[col] >= dx[col + 1] || dy[row] >= dy[row + 1]) continue;
        for (k = 0; k < 6; ++k) {
            int ix = col + corners[k][0], iy = row + corners[k][1];
            v[count].x = dx[ix]; v[count].y = dy[iy];
            v[count].z = 0.0f; v[count].rhw = 1.0f;
            if (falloff) {
                /* Original nine-slice cells retain their UV geometry.
                   Vary light down the panel, never at arbitrary screen edges. */
                float fraction = (dy[iy] - dy[0]) / (dy[3] - dy[0]);
                int a = (int)((float)((colour >> 24) & 255) * (1.0f - fraction));
                if (a < 0) a = 0;
                v[count].colour = (colour & 0x00FFFFFFu) | ((DWORD)a << 24);
            }
            else v[count].colour = colour;
            v[count].u = su[ix]; v[count].v = sv[iy];
            ++count;
        }
    }
    if (!count) return;
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTexture(0, t->tex);
    d->SetTextureStageState(0, D3DTSS_COLOROP, alphaMask ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE);
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
    d->SetVertexShader(UI_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, count / 3, v, sizeof(UiVert));
    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
}

void UI_DrawNineSlice(const Texture* t, float vx, float vy, float vw, float vh,
    float left, float top, float right, float bottom,
    DWORD colour, int additive) {
    ui_nineslice(t, vx, vy, vw, vh, left, top, right, bottom, colour, additive, 0, 0);
}

void UI_FillRect(float vx, float vy, float vw, float vh, DWORD colour) {
    ui_quad_mask(NULL, vx, vy, vw, vh, colour, 0, 0, 0);
}

/* additive solid fill (light/glow accents like the orb shimmer) */
void UI_FillRectAdd(float vx, float vy, float vw, float vh, DWORD colour) {
    ui_quad_mask(NULL, vx, vy, vw, vh, colour, 1, 0, 0);
}

/* Single-pass gradient made from per-vertex colour/alpha. Never depends on
   source texture alpha, so it cannot reintroduce opaque selection artifacts. */
void UI_FillGradient(float vx, float vy, float vw, float vh,
    DWORD topLeft, DWORD topRight, DWORD bottomLeft, DWORD bottomRight,
    int additive) {
    IDirect3DDevice8* d = Gfx_Device();
    UiVert v[4];
    float x0, x1, y0, y1;
    if (!d || vw <= 0.0f || vh <= 0.0f) return;
    x0 = UI_Sx(vx); y0 = UI_Sy(vy);
    x1 = UI_Sx(vx + vw); y1 = UI_Sy(vy + vh);
    v[0].x = x0; v[0].y = y0; v[0].z = 0.0f; v[0].rhw = 1.0f; v[0].colour = topLeft;     v[0].u = 0; v[0].v = 0;
    v[1].x = x1; v[1].y = y0; v[1].z = 0.0f; v[1].rhw = 1.0f; v[1].colour = topRight;    v[1].u = 0; v[1].v = 0;
    v[2].x = x0; v[2].y = y1; v[2].z = 0.0f; v[2].rhw = 1.0f; v[2].colour = bottomLeft;  v[2].u = 0; v[2].v = 0;
    v[3].x = x1; v[3].y = y1; v[3].z = 0.0f; v[3].rhw = 1.0f; v[3].colour = bottomRight; v[3].u = 0; v[3].v = 0;
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTexture(0, NULL);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(UI_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(UiVert));
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
}

/* Lightweight A6 panel-arrival tracker. Screen/popup panels are identified by
   their authored virtual rectangle, not by a new theme.ini key or asset.
   Each entry has a short warm-up glint on first appearance, and re-arms only
   after the panel has not been drawn for 450 ms. The plasma, fonts and layout
   never move. Capped storage (32 entries, no per-frame allocation). */
#if DD_A6_INTERACTION && DD_A4_DEPTH && DD_A5_LIGHTING
#define UI_ARRIVAL_SLOTS 32
#define UI_ARRIVAL_MS 240u
#define UI_ARRIVAL_RESET_MS 450u
typedef struct {
    int used, x, y, w, h, kind;
    DWORD started, lastSeen;
} UiArrival;
static UiArrival s_arrivals[UI_ARRIVAL_SLOTS];
static int ui_nearest(float v) { return v >= 0.0f ? (int)(v + 0.5f) : (int)(v - 0.5f); }
#endif

int UI_PanelArrivalAlpha(float vx, float vy, float vw, float vh, int kind) {
#if DD_A6_INTERACTION && DD_A4_DEPTH && DD_A5_LIGHTING
    DWORD now, age, elapsed, oldestAge = 0;
    int x, y, w, h, i, hit = -1, replacement = 0;
    if (!Data_FxOn(DD_FX_SELECT) || vw < 20.0f || vh < 20.0f) return 0;
    now = GetTickCount();
    x = ui_nearest(vx); y = ui_nearest(vy); w = ui_nearest(vw); h = ui_nearest(vh);
    for (i = 0; i < UI_ARRIVAL_SLOTS; i++) {
        UiArrival* a = &s_arrivals[i];
        if (a->used && a->x == x && a->y == y && a->w == w && a->h == h && a->kind == kind) {
            hit = i; break;
        }
        age = a->used ? now - a->lastSeen : 0xFFFFFFFFu;
        if (age > oldestAge) { oldestAge = age; replacement = i; }
    }
    if (hit < 0) {
        UiArrival* a = &s_arrivals[replacement];
        a->used = 1; a->x = x; a->y = y; a->w = w; a->h = h; a->kind = kind;
        a->started = now; a->lastSeen = now;
        return 22;
    }
    {
        UiArrival* a = &s_arrivals[hit];
        if (now - a->lastSeen > UI_ARRIVAL_RESET_MS) a->started = now;
        a->lastSeen = now;
        elapsed = now - a->started;
    }
    if (elapsed >= UI_ARRIVAL_MS) return 0;
    return (int)((22u * (UI_ARRIVAL_MS - elapsed)) / UI_ARRIVAL_MS);
#else
    (void)vx; (void)vy; (void)vw; (void)vh; (void)kind;
    return 0;
#endif
}

/* Above the artwork, below text and selections. No filled selection box,
   only a small directional catch light and one edge glint. */
   /* Procedural/unframed panels receive a feathered interior glow, rather
      than the old hard-edged 1-3 px top/side rails. The perimeter alpha is zero
      on all four sides: no visible straight-line light outline can remain. */
void UI_DrawPanelLighting(float vx, float vy, float vw, float vh, int opacity) {
#if DD_A4_DEPTH && DD_A5_LIGHTING
    IDirect3DDevice8* d = Gfx_Device();
    UiVert v[54];
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    float xs[4], ys[4];
    int r = (accent >> 16) & 255, g = (accent >> 8) & 255, b = accent & 255;
    int alpha = Data_EffectScale(DD_EFFECT_LIGHTING, 26 * opacity / 255);
    int n = 0, row, col, k;
    static const int corners[6][2] = { {0,0},{1,0},{0,1},{1,0},{1,1},{0,1} };
    if (!d || opacity <= 0 || alpha <= 0 || vw < 16.0f || vh < 16.0f) return;
    if (alpha > 26) alpha = 26;
    xs[0] = UI_Sx(vx);          xs[1] = UI_Sx(vx + vw * 0.18f);
    xs[2] = UI_Sx(vx + vw * 0.82f); xs[3] = UI_Sx(vx + vw);
    ys[0] = UI_Sy(vy);          ys[1] = UI_Sy(vy + vh * 0.18f);
    ys[2] = UI_Sy(vy + vh * 0.82f); ys[3] = UI_Sy(vy + vh);
    for (row = 0; row < 3; ++row) for (col = 0; col < 3; ++col) {
        for (k = 0; k < 6; ++k) {
            int ix = col + corners[k][0], iy = row + corners[k][1];
            int edge = (ix == 0 || ix == 3 || iy == 0 || iy == 3);
            v[n].x = xs[ix]; v[n].y = ys[iy]; v[n].z = 0.0f; v[n].rhw = 1.0f;
            v[n].colour = UI_ARGB(edge ? 0 : alpha, r, g, b);
            v[n].u = 0.0f; v[n].v = 0.0f;
            ++n;
        }
    }
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTexture(0, NULL);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(UI_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, v, sizeof(UiVert));
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
#else
    (void)vx; (void)vy; (void)vw; (void)vh; (void)opacity;
#endif
}

/* A single source-alpha-masked tinted pass; identical texel alpha to the
   original nine-slice or sprite, but ignores baked RGB for clean lighting.
   The vertical alpha fade is interpolated across the authored panel, avoiding
   any untextured rectangular light strips. */
#if DD_A4_DEPTH
static void ui_panel_mask_pass(const Texture* t, float vx, float vy, float vw, float vh,
    float left, float top, float right, float bottom, DWORD colour, int additive, int falloff) {
    if (!t || !t->tex) return;
    if (left > 0.0f || top > 0.0f || right > 0.0f || bottom > 0.0f)
        ui_nineslice(t, vx, vy, vw, vh, left, top, right, bottom, colour, additive, 1, falloff);
    else
        ui_quad_mask(t, vx, vy, vw, vh, colour, additive, 1, falloff);
}

#endif

void UI_DrawPanelLightingMasked(const Texture* t, float vx, float vy, float vw, float vh,
    int opacity, float left, float top, float right, float bottom) {
#if DD_A4_DEPTH && DD_A5_LIGHTING
    const ThemeStageLighting* l = Theme_StageLighting();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD emit = l ? l->emissive : accent;
    int material = l ? l->material : DD_STAGE_LIQUID;
    int r = (((accent >> 16) & 255) + ((emit >> 16) & 255)) / 2;
    int g = (((accent >> 8) & 255) + ((emit >> 8) & 255)) / 2;
    int b = ((accent & 255) + (emit & 255)) / 2;
    int a = material == DD_STAGE_HOLOGRAM ? 56 : (material == DD_STAGE_METAL ? 38 : 46);
    int level = l ? l->keyLevel : 100;
    if (!t || !t->tex || opacity <= 0) return;
    if (opacity > 255) opacity = 255;
    if (level > 150) level = 150;
    a = Data_EffectScale(DD_EFFECT_REFLECTION, a * opacity * level / (255 * 100));
    if (a <= 0) return;
    ui_panel_mask_pass(t, vx, vy, vw, vh, left, top, right, bottom,
        UI_ARGB(a, r, g, b), 1, 1);
#else
    (void)t; (void)vx; (void)vy; (void)vw; (void)vh;
    (void)opacity; (void)left; (void)top; (void)right; (void)bottom;
#endif
}

/* Soft shadow with a four-sided alpha feather. Each 3x3 mesh uses ONE
   triangle-list call. The outer ring fades to zero opacity, so there is no
   hard rectangle left on the backdrop. No blur texture or render target. */
#if DD_A4_DEPTH
static void ui_soft_shadow(float vx, float vy, float vw, float vh,
    int alpha, float offsetX, float offsetY, float feather) {
    IDirect3DDevice8* d = Gfx_Device();
    UiVert v[54];
    float sx[4], sy[4];
    int count = 0, row, col, k;
    static const int corners[6][2] = {
        {0,0}, {1,0}, {0,1}, {1,0}, {1,1}, {0,1}
    };
    if (!d || alpha <= 0 || vw <= 0.0f || vh <= 0.0f) return;
    if (feather > vw * 0.24f) feather = vw * 0.24f;
    if (feather > vh * 0.24f) feather = vh * 0.24f;
    if (feather < 1.0f) return;
    /* Feather outward from the panel bounds instead of inward+outward.
       This removes the visible rectangular inner edge that showed through
       transparent frame cutouts on authored panel art. */
    sx[0] = UI_Sx(vx + offsetX - feather);
    sx[1] = UI_Sx(vx + offsetX);
    sx[2] = UI_Sx(vx + offsetX + vw);
    sx[3] = UI_Sx(vx + offsetX + vw + feather);
    sy[0] = UI_Sy(vy + offsetY - feather);
    sy[1] = UI_Sy(vy + offsetY);
    sy[2] = UI_Sy(vy + offsetY + vh);
    sy[3] = UI_Sy(vy + offsetY + vh + feather);
    for (row = 0; row < 3; ++row) for (col = 0; col < 3; ++col) {
        for (k = 0; k < 6; ++k) {
            int ix = col + corners[k][0];
            int iy = row + corners[k][1];
            int edge = ix == 0 || ix == 3 || iy == 0 || iy == 3;
            v[count].x = sx[ix]; v[count].y = sy[iy];
            v[count].z = 0.0f; v[count].rhw = 1.0f;
            v[count].colour = UI_ARGB(edge ? 0 : alpha, 0, 0, 0);
            v[count].u = 0.0f; v[count].v = 0.0f;
            ++count;
        }
    }
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTexture(0, NULL);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(UI_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, count / 3, v, sizeof(UiVert));
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
}
#endif

/* Shared panel elevation for every flat submenu, dialog and file pane.
   Only the cast shadow changes; the theme artwork, lighting and text are
   drawn above this in their original order. */
void UI_DrawPanelDepth(float vx, float vy, float vw, float vh, int opacity) {
#if DD_A4_DEPTH
    float settle = 0.0f;
    int alpha;
    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;
    if (opacity == 0 || vw < 12.0f || vh < 12.0f) return;
#if DD_A6_INTERACTION && DD_A5_LIGHTING
    settle = (float)UI_PanelArrivalAlpha(vx, vy, vw, vh, 0) * (2.0f / 22.0f);
#endif
#if DD_VISUAL_IMPACT
    alpha = 52 * opacity / 255;
#else
    alpha = 36 * opacity / 255;
#endif
    alpha = Data_EffectScale(DD_EFFECT_SHADOW, alpha);
    ui_soft_shadow(vx, vy, vw, vh, alpha,
        4.0f + settle, 6.0f + settle, 12.0f);
#if DD_A5_LIGHTING
    {
        DWORD amb = Theme_StageLighting()->ambient;
        int lr = (amb >> 16) & 255, lg = (amb >> 8) & 255, lb = amb & 255;
        int a = Data_EffectScale(DD_EFFECT_ATMOSPHERE, 25 * opacity / 255);
        UI_FillGradient(vx + 2.0f, vy + 2.0f, vw - 4.0f, vh - 4.0f,
            UI_ARGB(a, lr, lg, lb), UI_ARGB(a / 3, lr, lg, lb),
            UI_ARGB(a / 4, lr, lg, lb), UI_ARGB(0, lr, lg, lb), 0);
    }
#else
    {
        DWORD accent = Theme_Color("accent_dim", 0xFF4C8A00);
        int r = (int)((accent >> 16) & 255);
        int g = (int)((accent >> 8) & 255);
        int b = (int)(accent & 255);
        UI_FillRect(vx + 3.0f, vy + 1.0f, vw - 6.0f, 1.0f,
            UI_ARGB(31 * opacity / 255, r, g, b));
    }
#endif
#else
    (void)vx; (void)vy; (void)vw; (void)vh; (void)opacity;
#endif
}

/* Soft, artwork-silhouette cast shadow for all textured flat panels.
   Each tap samples the same PNG alpha as the on-screen frame (including
   nine-slice UV mapping); transparent cutouts therefore stay transparent. */
void UI_DrawPanelDepthMasked(const Texture* t, float vx, float vy, float vw, float vh,
    int opacity, float left, float top, float right, float bottom) {
#if DD_A4_DEPTH
    struct ShadowTap { float dx, dy; int strength; };
    static const ShadowTap taps[] = {
        {0.0f, 0.0f, 32}, {-4.0f, 0.0f, 15}, {4.0f, 0.0f, 15},
        {0.0f, -4.0f, 15}, {0.0f, 4.0f, 15},
        {-6.0f, 5.0f, 8}, {6.0f, 5.0f, 8}, {0.0f, 8.0f, 8}
    };
    int i;
    if (opacity <= 0) return;
    if (!t || !t->tex) { UI_DrawPanelDepth(vx, vy, vw, vh, opacity); return; }
    if (opacity > 255) opacity = 255;
    for (i = 0; i < (int)(sizeof(taps) / sizeof(taps[0])); ++i) {
        int a = Data_EffectScale(DD_EFFECT_SHADOW, taps[i].strength * opacity / 255);
        if (a > 0) ui_panel_mask_pass(t, vx + 4.0f + taps[i].dx,
            vy + 6.0f + taps[i].dy, vw, vh,
            left, top, right, bottom, UI_ARGB(a, 0, 0, 0), 0, 0);
    }
#else
    (void)t; (void)vx; (void)vy; (void)vw; (void)vh;
    (void)opacity; (void)left; (void)top; (void)right; (void)bottom;
#endif
}

/* solid triangle from three virtual-space points (for corner markers etc.) */
void UI_FillTri(float ax, float ay, float bx, float by, float cx, float cy,
    DWORD colour) {
    IDirect3DDevice8* d = Gfx_Device();
    UiVert t[3];
    if (!d) return;

    t[0].x = UI_Sx(ax); t[0].y = UI_Sy(ay); t[0].z = 0; t[0].rhw = 1; t[0].colour = colour; t[0].u = 0; t[0].v = 0;
    t[1].x = UI_Sx(bx); t[1].y = UI_Sy(by); t[1].z = 0; t[1].rhw = 1; t[1].colour = colour; t[1].u = 0; t[1].v = 0;
    t[2].x = UI_Sx(cx); t[2].y = UI_Sy(cy); t[2].z = 0; t[2].rhw = 1; t[2].colour = colour; t[2].u = 0; t[2].v = 0;

    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetTexture(0, NULL);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(UI_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t, sizeof(UiVert));
}