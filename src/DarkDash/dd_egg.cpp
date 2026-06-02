/*---------------------------------------------------------------------------
    dd_egg.cpp -- spinning Darkone83 logo easter egg (see dd_egg.h).

    Mirrors the pedestal's D3DX quad pattern: a viewport over the target screen
    region, LookAtLH + PerspectiveFovLH, a Y-rotated world matrix, and one
    DrawPrimitiveUP of a textured quad. The logo loads from D:\data on first
    activation; on failure the egg simply won't show.

    Build: MSVC2003/C89 style; file-scope statics; d3dx8 (same as pedestal).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3dx8.h>
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "dd_egg.h"

#define EGG_PATH "D:\\data\\darkone83.png"
#define EGG_FVF  (D3DFVF_XYZ | D3DFVF_TEX1)
#define BEAM_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE)

/* the egg's light beam is always violet, regardless of theme */
#define EGG_BEAM_R 170
#define EGG_BEAM_G  60
#define EGG_BEAM_B 230

typedef struct { float x, y, z; float u, v; } EggVert;
typedef struct { float x, y, z; DWORD c; } EggBeam;

static int     s_active = 0;
static int     s_tried = 0;     /* attempted load? */
static Texture s_logo = { 0, 0, 0, 0, 0 };

void Egg_Toggle(void) {
    if (!s_active) {
        if (!s_tried) {            /* lazy-load the logo on first activation */
            s_tried = 1;
            if (!Texture_LoadPNG(EGG_PATH, &s_logo)) s_logo.tex = NULL;
        }
        if (s_logo.tex) s_active = 1;   /* only activate if the image is there */
    }
    else {
        s_active = 0;
    }
}

int Egg_Active(void) { return s_active && s_logo.tex != NULL; }

void Egg_Draw(int cx, int cy, int w, int h) {
    IDirect3DDevice8* d = Gfx_Device();
    const Texture* base;
    D3DVIEWPORT8 vpOld, vp;
    D3DXMATRIX   view, proj, world, rotY, ident;
    D3DXVECTOR3  eye, at, up;
    EggVert      q[6];
    EggBeam      beam[6];
    DWORD        ms, apexC, topC;
    int          apexA;
    float        ay, halfW, halfH, uMax, vMax;

    (void)cx; (void)cy; (void)w; (void)h;   /* egg uses the pedestal's placement */
    if (!Egg_Active() || !d) return;

    ms = GetTickCount();

    /* ---- empty-pedestal platform base (same sprite/placement as launcher) ---- */
    base = Theme_Asset("platform_round");
    if (base) UI_DrawSprite(base, 70.0f, 270.0f, 220.0f, 116.0f, 0xFFFFFFFF, 0);

    /* spin angle (~one rev / 6s) */
    ay = (float)(ms % 6000) / 6000.0f * 6.2831853f;

    /* viewport + camera copied from dd_pedestal so the logo floats ABOVE the
       platform and the beam fans down to meet it (NOT the lower orb region). */
    d->GetViewport(&vpOld);
    vp.X = (DWORD)UI_Sx(80.0f);
    vp.Y = (DWORD)UI_Sy(95.0f);
    vp.Width = (DWORD)UI_ScaleX(200.0f);
    vp.Height = (DWORD)UI_ScaleY(220.0f);
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    d->SetViewport(&vp);

    eye.x = 0.0f; eye.y = 0.0f; eye.z = -4.2f;
    at.x = 0.0f; at.y = 0.0f; at.z = 0.0f;
    up.x = 0.0f; up.y = 1.0f; up.z = 0.0f;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, 0.8f,
        (float)vp.Width / (float)vp.Height, 1.0f, 100.0f);
    d->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
    d->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);

    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    /* ---- 1) violet light shaft (additive), mirrors the pedestal beam ---- */
    apexA = 140 + (int)((ms >> 4) & 63);          /* gentle hologram pulse */
    apexC = ((DWORD)apexA << 24) | ((DWORD)EGG_BEAM_R << 16) | ((DWORD)EGG_BEAM_G << 8) | (DWORD)EGG_BEAM_B;
    topC = ((DWORD)0 << 24) | ((DWORD)EGG_BEAM_R << 16) | ((DWORD)EGG_BEAM_G << 8) | (DWORD)EGG_BEAM_B;
    beam[0].x = 0.0f;  beam[0].y = -1.85f; beam[0].z = 0.0f;  beam[0].c = apexC;
    beam[1].x = -0.9f; beam[1].y = -0.95f; beam[1].z = 0.0f;  beam[1].c = topC;
    beam[2].x = 0.9f;  beam[2].y = -0.95f; beam[2].z = 0.0f;  beam[2].c = topC;
    beam[3].x = 0.0f;  beam[3].y = -1.85f; beam[3].z = 0.0f;  beam[3].c = apexC;
    beam[4].x = 0.0f;  beam[4].y = -0.95f; beam[4].z = -0.9f; beam[4].c = topC;
    beam[5].x = 0.0f;  beam[5].y = -0.95f; beam[5].z = 0.9f;  beam[5].c = topC;

    D3DXMatrixIdentity(&ident);
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&ident);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);      /* additive = light */
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTexture(0, NULL);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(BEAM_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, beam, sizeof(EggBeam));

    /* ---- 2) the spinning logo quad (alpha-blended, both faces) ----
       Sized to roughly the cube's footprint and raised so its bottom sits just
       above the beam top (like the cube does), floating over the platform. */
    halfW = 1.25f; halfH = halfW * 2.0f / 3.0f;   /* ~0.83, 3:2 logo aspect */
    {
        float yc = 0.15f;                          /* lift to float above beam */
        D3DXMATRIX trans;
        D3DXMatrixRotationY(&rotY, ay);
        D3DXMatrixTranslation(&trans, 0.0f, yc, 0.0f);
        D3DXMatrixMultiply(&world, &rotY, &trans);
        d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);
    }

    uMax = (s_logo.pw > 0) ? (float)s_logo.w / (float)s_logo.pw : 1.0f;
    vMax = (s_logo.ph > 0) ? (float)s_logo.h / (float)s_logo.ph : 1.0f;
    {
        float x0 = -halfW, x1 = halfW, y0 = -halfH, y1 = halfH;
        q[0].x = x0; q[0].y = y1; q[0].z = 0; q[0].u = 0.0f; q[0].v = 0.0f;
        q[1].x = x1; q[1].y = y1; q[1].z = 0; q[1].u = uMax; q[1].v = 0.0f;
        q[2].x = x0; q[2].y = y0; q[2].z = 0; q[2].u = 0.0f; q[2].v = vMax;
        q[3].x = x1; q[3].y = y1; q[3].z = 0; q[3].u = uMax; q[3].v = 0.0f;
        q[4].x = x1; q[4].y = y0; q[4].z = 0; q[4].u = uMax; q[4].v = vMax;
        q[5].x = x0; q[5].y = y0; q[5].z = 0; q[5].u = 0.0f; q[5].v = vMax;
    }

    /* straight alpha blend so the keyed-out black stays transparent */
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    d->SetRenderState(D3DRS_ALPHAREF, 8);
    d->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    d->SetTexture(0, s_logo.tex);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    d->SetVertexShader(EGG_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(EggVert));

    /* ---- restore 2D-friendly state ---- */
    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    D3DXMatrixIdentity(&ident);
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&ident);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetViewport(&vpOld);
}

void Egg_Shutdown(void) {
    if (s_logo.tex) Texture_Release(&s_logo);
    s_logo.tex = NULL;
    s_active = 0; s_tried = 0;
}