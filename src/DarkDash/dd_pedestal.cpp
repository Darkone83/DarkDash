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
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_texture.h"
#include "dd_pedestal.h"

#define CUBE_FVF (D3DFVF_XYZ | D3DFVF_TEX1)
#define BEAM_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE)

typedef struct { float x, y, z, u, v; } CubeVert;
typedef struct { float x, y, z; DWORD c; } BeamVert;

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

/* flat billboard quad in the z=0 plane, faces the camera. Standard UVs (the
   icon is a normal PNG, not a swizzled XPR0, so no 180 rotation needed). */
static const CubeVert s_flat[6] = {
    {-0.8f,-0.8f, 0.0f, 0,1},{ 0.8f,-0.8f, 0.0f, 1,1},{ 0.8f, 0.8f, 0.0f, 1,0},
    {-0.8f,-0.8f, 0.0f, 0,1},{ 0.8f, 0.8f, 0.0f, 1,0},{-0.8f, 0.8f, 0.0f, 0,0}
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
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 12, s_cube, sizeof(CubeVert));

    /* ---- restore 2D-friendly state ---- */
    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetViewport(&vpOld);
}

/*---------------------------------------------------------------------------
    Pedestal_DrawFlat -- same platform + light shaft scheme as the cube
    version, but the artwork is a flat billboard quad facing the camera
    (used by the Settings screen for its category icon). 'icon' may be NULL.
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

    /* ---- flat icon quad: spins around Y like the cube it replaced ---- */
    ay = (float)((double)(ms % 7000u) * (6.2831853 / 7000.0));
    D3DXMatrixRotationY(&rY, ay);
    world = rY;
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);

    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTexture(0, icon->tex);
    d->SetVertexShader(CUBE_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, s_flat, sizeof(CubeVert));

    /* ---- restore 2D-friendly state ---- */
    d->SetTexture(0, NULL);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetViewport(&vpOld);
}