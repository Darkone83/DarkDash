/*---------------------------------------------------------------------------
    dd_ui.cpp -- virtual coordinate scaling + flat sprite/fill primitives.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_ui.h"
#include "dd_gfx.h"

static int   s_backW = 640;
static int   s_backH = 480;
static int   s_stretch = 0;       /* 0 = pillarbox (uniform), 1 = stretch (fill) */
static float s_scale_x = 1.0f;
static float s_scale_y = 1.0f;
static float s_off_x = 0.0f;
static float s_off_y = 0.0f;

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
static void ui_quad(const Texture* tex,
    float vx, float vy, float vw, float vh,
    DWORD colour, int additive) {
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

    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);

    if (tex && tex->tex) {
        d->SetTexture(0, tex->tex);
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
    ui_quad(t, vx, vy, vw, vh, colour, additive);
}

void UI_DrawSpriteNative(const Texture* t, float vx, float vy,
    DWORD colour, int additive) {
    if (!t || !t->tex) return;
    ui_quad(t, vx, vy, (float)t->w, (float)t->h, colour, additive);
}

void UI_FillRect(float vx, float vy, float vw, float vh, DWORD colour) {
    ui_quad(NULL, vx, vy, vw, vh, colour, 0);
}