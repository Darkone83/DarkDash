/*---------------------------------------------------------------------------
    dd_pedestal.cpp -- spinning title-image cube + a real light shaft.

    The title texture maps onto a slowly spinning cube (lighting off, so the
    logo stays crisp -- it reads as the projected hologram). Beneath it, an
    EMISSIVE light cone is drawn as additive 3D geometry in the same scene:
    a narrow bright point at the pedestal fanning upward and dissipating
    before the cube. It is not a 2D sprite -- it lives in perspective with the
    cube, so it looks like a projector beam, and additive blending makes it
    behave like emitted light rather than a pasted texture.

    Cube UVs are rotated 180 (U and V both flipped) so the art reads upright
    and correct, not mirrored.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>
#include <d3dx8.h>
#include <math.h>
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_texture.h"
#include "dd_pedestal.h"

#define CUBE_FVF (D3DFVF_XYZ | D3DFVF_TEX1)
#define BEAM_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE)
#define HOLO_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct { float x, y, z, u, v; } CubeVert;
typedef struct { float x, y, z; DWORD c; } BeamVert;
typedef struct { float x, y, z; DWORD c; float u, v; } HoloVert;

/* unit cube (half = 0.9), full texture per face, UV rotated 180 so the art is
   upright and reads correctly. CULL NONE + Z-buffer sorts the faces. */
static const CubeVert s_cube[36] = {
    /* front +Z */
    {-0.9f,-0.9f, 0.9f, 1,1},{ 0.9f,-0.9f, 0.9f, 0,1},{ 0.9f, 0.9f, 0.9f, 0,0},
    {-0.9f,-0.9f, 0.9f, 1,1},{ 0.9f, 0.9f, 0.9f, 0,0},{-0.9f, 0.9f, 0.9f, 1,0},
    /* back -Z */
    { 0.9f,-0.9f,-0.9f, 1,1},{-0.9f,-0.9f,-0.9f, 0,1},{-0.9f, 0.9f,-0.9f, 0,0},
    { 0.9f,-0.9f,-0.9f, 1,1},{-0.9f, 0.9f,-0.9f, 0,0},{ 0.9f, 0.9f,-0.9f, 1,0},
    /* right +X */
    { 0.9f,-0.9f, 0.9f, 1,1},{ 0.9f,-0.9f,-0.9f, 0,1},{ 0.9f, 0.9f,-0.9f, 0,0},
    { 0.9f,-0.9f, 0.9f, 1,1},{ 0.9f, 0.9f,-0.9f, 0,0},{ 0.9f, 0.9f, 0.9f, 1,0},
    /* left -X */
    {-0.9f,-0.9f,-0.9f, 1,1},{-0.9f,-0.9f, 0.9f, 0,1},{-0.9f, 0.9f, 0.9f, 0,0},
    {-0.9f,-0.9f,-0.9f, 1,1},{-0.9f, 0.9f, 0.9f, 0,0},{-0.9f, 0.9f,-0.9f, 1,0},
    /* top +Y */
    {-0.9f, 0.9f, 0.9f, 1,1},{ 0.9f, 0.9f, 0.9f, 0,1},{ 0.9f, 0.9f,-0.9f, 0,0},
    {-0.9f, 0.9f, 0.9f, 1,1},{ 0.9f, 0.9f,-0.9f, 0,0},{-0.9f, 0.9f,-0.9f, 1,0},
    /* bottom -Y */
    {-0.9f,-0.9f,-0.9f, 1,1},{ 0.9f,-0.9f,-0.9f, 0,1},{ 0.9f,-0.9f, 0.9f, 0,0},
    {-0.9f,-0.9f,-0.9f, 1,1},{ 0.9f,-0.9f, 0.9f, 0,0},{-0.9f,-0.9f, 0.9f, 1,0}
};

void Pedestal_Draw(const Texture* art, const Texture* glow,
    DWORD ms, int ar, int ag, int ab) {
    IDirect3DDevice8* d = Gfx_Device();
    D3DVIEWPORT8 vpOld, vpCube;
    D3DXMATRIX   world, view, proj, ident, rY, rX;
    D3DXVECTOR3  eye, at, up;
    float        ay;
    int          apexA;
    DWORD        apexC, topC;
    BeamVert     beam[6];

    (void)glow;
    if (!d || !art || !art->tex) return;

    d->GetViewport(&vpOld);
    /* viewport authored in 640x480 logical space -> physical via UI_Sx/UI_Sy
       so it lands correctly (and stays proportional) at 720p too */
    vpCube.X = (DWORD)UI_Sx(80.0f);
    vpCube.Y = (DWORD)UI_Sy(95.0f);
    vpCube.Width = (DWORD)UI_ScaleX(200.0f);
    vpCube.Height = (DWORD)UI_ScaleY(220.0f);
    vpCube.MinZ = 0.0f; vpCube.MaxZ = 1.0f;
    d->SetViewport(&vpCube);

    eye.x = 0.0f; eye.y = 0.0f; eye.z = -4.2f;
    at.x = 0.0f; at.y = 0.0f; at.z = 0.0f;
    up.x = 0.0f; up.y = 1.0f; up.z = 0.0f;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, 0.8f,
        (float)vpCube.Width / (float)vpCube.Height, 1.0f, 100.0f);
    d->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
    d->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);

    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    /* ---- 1) emissive light shaft (additive, fixed, behind the cube) ---- */
    apexA = 140 + (int)((ms >> 4) & 63);          /* gentle hologram pulse */
    apexC = ((DWORD)apexA << 24) | ((DWORD)ar << 16) | ((DWORD)ag << 8) | (DWORD)ab;
    topC = ((DWORD)0 << 24) | ((DWORD)ar << 16) | ((DWORD)ag << 8) | (DWORD)ab;
    /* two crossed triangles: narrow bright apex at the pedestal, fanning up
       and fading to nothing just under the cube */
    beam[0].x = 0.0f; beam[0].y = -1.85f; beam[0].z = 0.0f; beam[0].c = apexC;
    beam[1].x = -0.9f; beam[1].y = -0.95f; beam[1].z = 0.0f; beam[1].c = topC;
    beam[2].x = 0.9f; beam[2].y = -0.95f; beam[2].z = 0.0f; beam[2].c = topC;
    beam[3].x = 0.0f; beam[3].y = -1.85f; beam[3].z = 0.0f; beam[3].c = apexC;
    beam[4].x = 0.0f; beam[4].y = -0.95f; beam[4].z = -0.9f; beam[4].c = topC;
    beam[5].x = 0.0f; beam[5].y = -0.95f; beam[5].z = 0.9f; beam[5].c = topC;

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
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, beam, sizeof(BeamVert));

    /* ---- 2) the spinning cube (opaque, crisp, z-sorted) ---- */
    ay = (float)((double)(ms % 7000u) * (6.2831853 / 7000.0));
    D3DXMatrixRotationY(&rY, ay);
    D3DXMatrixRotationX(&rX, -0.5f);
    D3DXMatrixMultiply(&world, &rY, &rX);
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);

    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTexture(0, art->tex);
    d->SetVertexShader(CUBE_FVF);
    {
        /* s_cube has 0/1 UVs assuming the art fills the texture. A real XBE
           title image is a pow2 square so that's already right; but a generic
           placeholder PNG may be non-pow2 (padded -> art in the top-left
           fraction) or non-square. Remap the per-face UVs to the real image
           region (w/pw, h/ph) so it fills each face centered, not shrunk into a
           corner with the transparent pad showing. */
        CubeVert cv[36];
        float umax = (art->pw > 0) ? (float)art->w / (float)art->pw : 1.0f;
        float vmax = (art->ph > 0) ? (float)art->h / (float)art->ph : 1.0f;
        int n;
        for (n = 0; n < 36; n++) {
            cv[n] = s_cube[n];
            cv[n].u = (s_cube[n].u > 0.5f) ? umax : 0.0f;
            cv[n].v = (s_cube[n].v > 0.5f) ? vmax : 0.0f;
        }
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 12, cv, sizeof(CubeVert));
    }

    /* ---- restore 2D-friendly state ---- */
    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetViewport(&vpOld);
}

/*---------------------------------------------------------------------------
    Pedestal_DrawHologram -- same platform + light shaft scheme, but the art is
    a translucent, flickering, gently-swaying hologram. Used by the launcher for
    _resources case art. 'icon' may be NULL.
---------------------------------------------------------------------------*/
void Pedestal_DrawHologram(const Texture* icon, DWORD ms, int ar, int ag, int ab) {
    IDirect3DDevice8* d = Gfx_Device();
    D3DVIEWPORT8 vpOld, vpCube;
    D3DXMATRIX   view, proj, ident, world, rY;
    D3DXVECTOR3  eye, at, up;
    int          apexA;
    DWORD        apexC, topC;
    float        ay;
    BeamVert     beam[6];

    if (!d || !icon || !icon->tex) return;

    d->GetViewport(&vpOld);
    /* viewport authored in 640x480 logical space -> physical via UI_Sx/UI_Sy
       so it lands correctly (and stays proportional) at 720p too */
    vpCube.X = (DWORD)UI_Sx(80.0f);
    vpCube.Y = (DWORD)UI_Sy(95.0f);
    vpCube.Width = (DWORD)UI_ScaleX(200.0f);
    vpCube.Height = (DWORD)UI_ScaleY(220.0f);
    vpCube.MinZ = 0.0f; vpCube.MaxZ = 1.0f;
    d->SetViewport(&vpCube);

    eye.x = 0.0f; eye.y = 0.0f; eye.z = -4.2f;
    at.x = 0.0f; at.y = 0.0f; at.z = 0.0f;
    up.x = 0.0f; up.y = 1.0f; up.z = 0.0f;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, 0.8f,
        (float)vpCube.Width / (float)vpCube.Height, 1.0f, 100.0f);
    d->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
    d->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);

    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    /* ---- light shaft (identical to the cube version) ---- */
    apexA = 140 + (int)((ms >> 4) & 63);
    apexC = ((DWORD)apexA << 24) | ((DWORD)ar << 16) | ((DWORD)ag << 8) | (DWORD)ab;
    topC = ((DWORD)0 << 24) | ((DWORD)ar << 16) | ((DWORD)ag << 8) | (DWORD)ab;
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
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);       /* additive = light */
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTexture(0, NULL);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(BEAM_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, beam, sizeof(BeamVert));

    /* ---- holographic icon: gentle sway, translucent, flickering ---- */
    /* gentle side-to-side sway (~+/-12 deg), never a full spin so it faces us */
    ay = (float)(0.21 * sin((double)ms * (6.2831853 / 4200.0)));
    D3DXMatrixRotationY(&rY, ay);
    world = rY;
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);

    /* flicker: a calm base alpha with a fast low-amplitude jitter plus an
       occasional brief dropout, so it reads as an unstable projection. The
       jitter is a cheap hash of the time tick (no rand(), deterministic). */
    {
        DWORD t = ms;
        unsigned h = (unsigned)(t * 2654435761u);   /* Knuth multiplicative */
        int jitter = (int)((h >> 24) & 0x1F);        /* 0..31 */
        int base = 178;                              /* ~70% translucent */
        int a = base - jitter;                       /* small downward wobble */
        DWORD col;
        HoloVert hv[6];
        float hx, hy, umax, vmax;
        /* occasional deeper flicker dip (~ every 1.2s for a brief moment) */
        if (((t / 60u) % 20u) == 0u) a -= 70;
        if (a < 70)  a = 70;
        if (a > 200) a = 200;
        /* tint slightly toward the theme accent so it looks projected, not a
           photo: blend the art's own colour with the accent via diffuse. */
        {
            int tr = (ar + 255) / 2, tg = (ag + 255) / 2, tb = (ab + 255) / 2;
            col = ((DWORD)a << 24) | ((DWORD)tr << 16) | ((DWORD)tg << 8) | (DWORD)tb;
        }

        /* size the quad to the art's real aspect ratio, fit inside a target box
           (~1.5 half-extent) so a tall DVD-case cover fills the pedestal instead
           of rendering as a tiny square. Then map UVs to the real (unpadded)
           region of the pow2 texture so we never sample the transparent pad. */
        {
            float aw = (icon->w > 0) ? (float)icon->w : 1.0f;
            float ah = (icon->h > 0) ? (float)icon->h : 1.0f;
            float aspect = aw / ah;          /* >1 wide, <1 tall */
            float fit = 1.5f;                /* half-extent of the fit box */
            if (aspect >= 1.0f) { hx = fit;          hy = fit / aspect; }
            else { hx = fit * aspect; hy = fit; }
        }
        umax = (icon->pw > 0) ? (float)icon->w / (float)icon->pw : 1.0f;
        vmax = (icon->ph > 0) ? (float)icon->h / (float)icon->ph : 1.0f;

        /* two triangles, CCW, facing +Z (camera). UV origin top-left. */
        hv[0].x = -hx; hv[0].y = -hy; hv[0].z = 0; hv[0].u = 0;    hv[0].v = vmax;
        hv[1].x = hx; hv[1].y = -hy; hv[1].z = 0; hv[1].u = umax; hv[1].v = vmax;
        hv[2].x = hx; hv[2].y = hy; hv[2].z = 0; hv[2].u = umax; hv[2].v = 0;
        hv[3].x = -hx; hv[3].y = -hy; hv[3].z = 0; hv[3].u = 0;    hv[3].v = vmax;
        hv[4].x = hx; hv[4].y = hy; hv[4].z = 0; hv[4].u = umax; hv[4].v = 0;
        hv[5].x = -hx; hv[5].y = hy; hv[5].z = 0; hv[5].u = 0;    hv[5].v = 0;
        hv[0].c = hv[1].c = hv[2].c = hv[3].c = hv[4].c = hv[5].c = col;

        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);   /* additive = glowy hologram */
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        /* texture * diffuse: diffuse carries the tint + flicker alpha */
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d->SetTexture(0, icon->tex);
        d->SetVertexShader(HOLO_FVF);
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, hv, sizeof(HoloVert));
    }

    /* ---- restore 2D-friendly state ---- */
    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetViewport(&vpOld);
}

/*---------------------------------------------------------------------------
    Pedestal_DrawFlat -- plain opaque flat quad, gently spinning around Y.
    The Settings category icon. No hologram flicker/tint. 'icon' may be NULL.
---------------------------------------------------------------------------*/
void Pedestal_DrawFlat(const Texture* icon, DWORD ms, int ar, int ag, int ab) {
    IDirect3DDevice8* d = Gfx_Device();
    D3DVIEWPORT8 vpOld, vpCube;
    D3DXMATRIX   view, proj, ident, world, rY;
    D3DXVECTOR3  eye, at, up;
    int          apexA;
    DWORD        apexC, topC;
    float        ay;
    BeamVert     beam[6];

    if (!d || !icon || !icon->tex) return;

    d->GetViewport(&vpOld);
    vpCube.X = (DWORD)UI_Sx(80.0f);
    vpCube.Y = (DWORD)UI_Sy(95.0f);
    vpCube.Width = (DWORD)UI_ScaleX(200.0f);
    vpCube.Height = (DWORD)UI_ScaleY(220.0f);
    vpCube.MinZ = 0.0f; vpCube.MaxZ = 1.0f;
    d->SetViewport(&vpCube);

    eye.x = 0.0f; eye.y = 0.0f; eye.z = -4.2f;
    at.x = 0.0f; at.y = 0.0f; at.z = 0.0f;
    up.x = 0.0f; up.y = 1.0f; up.z = 0.0f;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, 0.8f,
        (float)vpCube.Width / (float)vpCube.Height, 1.0f, 100.0f);
    d->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
    d->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);

    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    /* ---- light shaft ---- */
    apexA = 140 + (int)((ms >> 4) & 63);
    apexC = ((DWORD)apexA << 24) | ((DWORD)ar << 16) | ((DWORD)ag << 8) | (DWORD)ab;
    topC = ((DWORD)0 << 24) | ((DWORD)ar << 16) | ((DWORD)ag << 8) | (DWORD)ab;
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
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);       /* additive = light */
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTexture(0, NULL);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(BEAM_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, beam, sizeof(BeamVert));

    /* ---- flat icon quad: spins around Y, opaque, aspect-correct ---- */
    ay = (float)((double)(ms % 7000u) * (6.2831853 / 7000.0));
    D3DXMatrixRotationY(&rY, ay);
    world = rY;
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);

    {
        CubeVert q[6];
        float hx, hy, umax, vmax;
        float aw = (icon->w > 0) ? (float)icon->w : 1.0f;
        float ah = (icon->h > 0) ? (float)icon->h : 1.0f;
        float aspect = aw / ah;
        float fit = 0.9f;                    /* match the cube's half-extent */
        if (aspect >= 1.0f) { hx = fit;          hy = fit / aspect; }
        else { hx = fit * aspect; hy = fit; }
        umax = (icon->pw > 0) ? (float)icon->w / (float)icon->pw : 1.0f;
        vmax = (icon->ph > 0) ? (float)icon->h / (float)icon->ph : 1.0f;
        q[0].x = -hx; q[0].y = -hy; q[0].z = 0; q[0].u = 0;    q[0].v = vmax;
        q[1].x = hx; q[1].y = -hy; q[1].z = 0; q[1].u = umax; q[1].v = vmax;
        q[2].x = hx; q[2].y = hy; q[2].z = 0; q[2].u = umax; q[2].v = 0;
        q[3].x = -hx; q[3].y = -hy; q[3].z = 0; q[3].u = 0;    q[3].v = vmax;
        q[4].x = hx; q[4].y = hy; q[4].z = 0; q[4].u = umax; q[4].v = 0;
        q[5].x = -hx; q[5].y = hy; q[5].z = 0; q[5].u = 0;    q[5].v = 0;

        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);   /* normal alpha */
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d->SetTexture(0, icon->tex);
        d->SetVertexShader(CUBE_FVF);
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(CubeVert));
    }

    /* ---- restore 2D-friendly state ---- */
    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetViewport(&vpOld);
}

/*---------------------------------------------------------------------------
    Pedestal_DrawSaver -- screensaver variant. Same pedestal + light shaft, but:
      - the viewport is shifted by (dxV,dyV) virtual px so the whole pedestal
        slowly drifts around the screen (burn-in safe),
      - 'fadeA' (0..255) scales both the art alpha and the beam intensity so the
        title can fade in and out,
      - the beam colour is taken directly from (br,bg,bb) -- the caller cycles
        this through an RGB rainbow.
    isFlat=1 draws the art as a hologram (opencase art); isFlat=0 draws it on
    the spinning cube (title image / placeholder). 'icon' may be NULL.
---------------------------------------------------------------------------*/
void Pedestal_DrawSaver(const Texture* icon, int isFlat, DWORD ms,
    float dxV, float dyV, int fadeA,
    int br, int bg, int bb) {
    IDirect3DDevice8* d = Gfx_Device();
    D3DVIEWPORT8 vpOld, vpCube;
    D3DXMATRIX   view, proj, ident, world, rY, rX;
    D3DXVECTOR3  eye, at, up;
    int          apexA;
    DWORD        apexC, topC;
    float        ay;
    BeamVert     beam[6];

    if (!d || !icon || !icon->tex) return;
    if (fadeA < 0) fadeA = 0; if (fadeA > 255) fadeA = 255;

    d->GetViewport(&vpOld);
    vpCube.X = (DWORD)UI_Sx(80.0f + dxV);
    vpCube.Y = (DWORD)UI_Sy(95.0f + dyV);
    vpCube.Width = (DWORD)UI_ScaleX(200.0f);
    vpCube.Height = (DWORD)UI_ScaleY(220.0f);
    vpCube.MinZ = 0.0f; vpCube.MaxZ = 1.0f;
    d->SetViewport(&vpCube);

    eye.x = 0.0f; eye.y = 0.0f; eye.z = -4.2f;
    at.x = 0.0f; at.y = 0.0f; at.z = 0.0f;
    up.x = 0.0f; up.y = 1.0f; up.z = 0.0f;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, 0.8f,
        (float)vpCube.Width / (float)vpCube.Height, 1.0f, 100.0f);
    d->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
    d->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);

    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    /* ---- rainbow light shaft, faded ---- */
    apexA = (140 + (int)((ms >> 4) & 63)) * fadeA / 255;
    apexC = ((DWORD)apexA << 24) | ((DWORD)br << 16) | ((DWORD)bg << 8) | (DWORD)bb;
    topC = ((DWORD)0 << 24) | ((DWORD)br << 16) | ((DWORD)bg << 8) | (DWORD)bb;
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
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTexture(0, NULL);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(BEAM_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, beam, sizeof(BeamVert));

    /* ---- the art ---- */
    if (isFlat) {
        /* hologram billboard, swaying, faded */
        DWORD col; HoloVert hv[6];
        float hx, hy, umax, vmax;
        int a;
        ay = (float)(0.21 * sin((double)ms * (6.2831853 / 4200.0)));
        D3DXMatrixRotationY(&rY, ay);
        world = rY;
        d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);

        a = 178 * fadeA / 255;       /* base translucency scaled by fade */
        {
            int tr = (br + 255) / 2, tg = (bg + 255) / 2, tb = (bb + 255) / 2;
            col = ((DWORD)a << 24) | ((DWORD)tr << 16) | ((DWORD)tg << 8) | (DWORD)tb;
        }
        {
            float aw = (icon->w > 0) ? (float)icon->w : 1.0f;
            float ah = (icon->h > 0) ? (float)icon->h : 1.0f;
            float aspect = aw / ah;
            float fit = 1.5f;
            if (aspect >= 1.0f) { hx = fit;          hy = fit / aspect; }
            else { hx = fit * aspect; hy = fit; }
        }
        umax = (icon->pw > 0) ? (float)icon->w / (float)icon->pw : 1.0f;
        vmax = (icon->ph > 0) ? (float)icon->h / (float)icon->ph : 1.0f;
        hv[0].x = -hx; hv[0].y = -hy; hv[0].z = 0; hv[0].u = 0;    hv[0].v = vmax;
        hv[1].x = hx; hv[1].y = -hy; hv[1].z = 0; hv[1].u = umax; hv[1].v = vmax;
        hv[2].x = hx; hv[2].y = hy; hv[2].z = 0; hv[2].u = umax; hv[2].v = 0;
        hv[3].x = -hx; hv[3].y = -hy; hv[3].z = 0; hv[3].u = 0;    hv[3].v = vmax;
        hv[4].x = hx; hv[4].y = hy; hv[4].z = 0; hv[4].u = umax; hv[4].v = 0;
        hv[5].x = -hx; hv[5].y = hy; hv[5].z = 0; hv[5].u = 0;    hv[5].v = 0;
        hv[0].c = hv[1].c = hv[2].c = hv[3].c = hv[4].c = hv[5].c = col;

        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d->SetTexture(0, icon->tex);
        d->SetVertexShader(HOLO_FVF);
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, hv, sizeof(HoloVert));
    }
    else {
        /* spinning cube. CubeVert has no diffuse, and a title image is opaque,
           so fade via TEXTUREFACTOR alpha: color straight from the texture,
           alpha taken from the constant factor (= fadeA). */
        CubeVert cv[36];
        float umax = (icon->pw > 0) ? (float)icon->w / (float)icon->pw : 1.0f;
        float vmax = (icon->ph > 0) ? (float)icon->h / (float)icon->ph : 1.0f;
        int n;
        ay = (float)((double)(ms % 7000u) * (6.2831853 / 7000.0));
        D3DXMatrixRotationY(&rY, ay);
        D3DXMatrixRotationX(&rX, -0.5f);
        D3DXMatrixMultiply(&world, &rY, &rX);
        d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);
        for (n = 0; n < 36; n++) {
            cv[n] = s_cube[n];
            cv[n].u = (s_cube[n].u > 0.5f) ? umax : 0.0f;
            cv[n].v = (s_cube[n].v > 0.5f) ? vmax : 0.0f;
        }
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        /* The cube is solid 3D geometry, so it needs depth testing or its 6
           faces blend through each other (the "see-through / all faces" look).
           Enable Z-test+write and clear depth in this viewport for a clean
           slate (the field/backdrop left stale depth here). The constant-alpha
           fade still applies -- it dims the whole solid cube uniformly. */
        d->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
        d->SetRenderState(D3DRS_ZENABLE, TRUE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        d->SetRenderState(D3DRS_TEXTUREFACTOR, ((DWORD)fadeA << 24) | 0x00FFFFFF);
        /* color from texture; alpha from the constant factor (the fade) */
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        d->SetTexture(0, icon->tex);
        d->SetVertexShader(CUBE_FVF);
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 12, cv, sizeof(CubeVert));
    }

    /* ---- restore ---- */
    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetViewport(&vpOld);
}