/*---------------------------------------------------------------------------
    dd_swing.cpp -- door-swing transition (see dd_swing.h).

    The console is captured to an offscreen render target, then that texture is
    mapped onto a quad that rotates around its RIGHT edge (hinge). Because the
    whole console (frame + rows + highlight) is in the texture, the text and
    highlight turn WITH the door -- no cross-fade.

    Motion is deliberately mechanical, not a spin:
        OUT   swing 0 -> 90deg (edge-on, flat to viewer), ease-out into the stop
        DWELL hold dead-still at edge-on (content swap happens here, hidden)
        BACK  swing 90deg -> 0, ease-out into the closed stop

    RTT note: render targets on the NV2A are linear (unswizzled); this creates
    a full-frame-sized A8R8G8B8 target + matching depth, renders the console
    into it via the normal iso calls (no coord remap), then samples the
    console's sub-rect onto the swinging quad.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3dx8.h>
#include "dd_swing.h"
#include "dd_gfx.h"
#include "dd_ui.h"

#define SWING_FVF (D3DFVF_XYZ | D3DFVF_TEX1)

typedef struct { float x, y, z; float u, v; } SwingVert;

/* --- timeline (ms): rect fades out, THEN the door (frame+text) swings out,
   dwells edge-on, swings back; once closed the rect fades back in. The rect is
   never part of the swung texture -- it's a separate fade bracketing the swing,
   which is what gives the deliberate "release then turn" feel. --- */
#define RECT_OUT_MS     110.0f
#define SWING_OUT_MS    290.0f
#define SWING_DWELL_MS  200.0f
#define SWING_BACK_MS   290.0f
#define RECT_IN_MS      110.0f

#define T_RECTOUT_END   (RECT_OUT_MS)
#define T_SWINGOUT_END  (T_RECTOUT_END  + SWING_OUT_MS)
#define T_DWELL_END     (T_SWINGOUT_END + SWING_DWELL_MS)
#define T_SWINGBACK_END (T_DWELL_END    + SWING_BACK_MS)
#define SWING_TOTAL_MS  (T_SWINGBACK_END + RECT_IN_MS)

#define SWING_MAXANG    1.5707963f   /* 90 deg: edge-on, flat to the viewer */

   /* --- animation state --- */
static int   s_active = 0;
static float s_elapsed = 0.0f;      /* ms since Swing_Start            */
static int   s_midpoint = 0;         /* latched true for one frame      */
static int   s_didMid = 0;         /* midpoint already fired?         */
static int   s_dir = 0;         /* 0 = full sequence, 1 = in-only  */

/* --- RTT resources (created lazily, full-frame sized) --- */
static IDirect3DTexture8* s_rt = NULL;   /* render-target texture */
static IDirect3DSurface8* s_rtSurf = NULL;   /* its surface level 0   */
static IDirect3DSurface8* s_rtDepth = NULL;   /* matching depth        */
static IDirect3DSurface8* s_saveCol = NULL;   /* saved backbuffer      */
static IDirect3DSurface8* s_saveZ = NULL;   /* saved depth           */
static int  s_rtW = 0, s_rtH = 0;
static int  s_capturing = 0;

/* ---------------------------------------------------------------------------
   Lifecycle / timeline
---------------------------------------------------------------------------*/

void Swing_Start(void) {
    s_active = 1;
    s_elapsed = 0.0f;
    s_midpoint = 0;
    s_didMid = 0;
    s_dir = 0;                   /* full sequence (entering) */
}

/* return path: the new (main) menu arrives. Skip the rect-out + swing-out and
   jump to the dwell, so it plays dwell -> swing-back -> rect-in: the door
   swings in with the menu and the rect fades in once it lands. Content is
   already the main menu, so no midpoint swap needed. */
void Swing_StartIn(void) {
    s_active = 1;
    s_elapsed = T_SWINGOUT_END;      /* begin at the dwell */
    s_midpoint = 0;
    s_didMid = 1;
    s_dir = 1;
}

int Swing_Active(void) { return s_active; }

void Swing_Update(DWORD dtMs) {
    if (!s_active) return;
    s_midpoint = 0;
    s_elapsed += (float)dtMs;

    /* swap content once, during the dwell (door edge-on/invisible) */
    if (!s_didMid && s_elapsed >= T_SWINGOUT_END) {
        s_didMid = 1;
        s_midpoint = 1;
    }
    if (s_elapsed >= SWING_TOTAL_MS) {
        s_elapsed = SWING_TOTAL_MS;
        s_active = 0;
    }
}

int Swing_TookMidpoint(void) { return s_midpoint; }

/* ease-out: decelerate into the stop (1 - (1-t)^2). t in 0..1 */
static float EaseOut(float t) {
    float inv;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    inv = 1.0f - t;
    return 1.0f - inv * inv;
}

/* hinge angle from the timeline (0 closed .. SWING_MAXANG edge-on). Flat (0)
   during the rect-fade phases at each end. */
static float Swing_Angle(void) {
    if (s_elapsed < T_RECTOUT_END)   return 0.0f;                  /* rect fading out */
    if (s_elapsed < T_SWINGOUT_END) {                             /* opening         */
        float t = EaseOut((s_elapsed - T_RECTOUT_END) / SWING_OUT_MS);
        return t * SWING_MAXANG;
    }
    if (s_elapsed < T_DWELL_END)     return SWING_MAXANG;          /* dwell           */
    if (s_elapsed < T_SWINGBACK_END) {                           /* closing         */
        float t = EaseOut((s_elapsed - T_DWELL_END) / SWING_BACK_MS);
        return (1.0f - t) * SWING_MAXANG;
    }
    return 0.0f;                                                  /* rect fading in  */
}

/* the door (frame+text) is turning only during out/dwell/back -- NOT during the
   rect-fade phases at each end. The renderer captures+swings only then. */
int Swing_Doorturning(void) {
    if (!s_active) return 0;
    return (s_elapsed >= T_RECTOUT_END && s_elapsed < T_SWINGBACK_END) ? 1 : 0;
}

/* selection-rect alpha 0..255: full at rest; fades OUT over RECT_OUT at the
   start, stays 0 through the whole swing, fades IN over RECT_IN at the end. */
int Swing_RectAlpha(void) {
    float a;
    if (!s_active) return 255;
    if (s_elapsed < T_RECTOUT_END) {
        a = 1.0f - (s_elapsed / RECT_OUT_MS);            /* 1 -> 0 */
    }
    else if (s_elapsed < T_SWINGBACK_END) {
        a = 0.0f;                                        /* hidden through swing */
    }
    else {
        a = (s_elapsed - T_SWINGBACK_END) / RECT_IN_MS;  /* 0 -> 1 */
    }
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return (int)(a * 255.0f);
}

/* legacy; content is in the swung texture now -- always opaque */
int Swing_ContentAlpha(void) { return 255; }

/* ---------------------------------------------------------------------------
   RTT capture

   The console is captured into a render target sized to the console's own
   pixel footprint (not the whole frame), so it's captured at native density
   and sampled 1:1 onto the swinging quad -- no sub-rect resample blur.

   The console is drawn in full-frame virtual coords via the iso projection,
   so to make it land inside the small RT we set an OFFSET viewport: the
   viewport keeps full-frame dimensions (so the iso projection is unchanged)
   but is shifted by -console_origin, sliding the console into the RT's 0,0.
   A little padding catches the iso tilt's overhang.
---------------------------------------------------------------------------*/

#define CAP_PAD 24                    /* px padding for the iso tilt overhang */

/* console rect in virtual space + its captured pixel size (set in Begin) */
static int s_capX = 0, s_capY = 0, s_capW = 0, s_capH = 0;   /* RT-space px */

static int EnsureTargets(IDirect3DDevice8* d, int w, int h) {
    if (s_rt && s_rtW == w && s_rtH == h) return 1;

    if (s_rtSurf) { s_rtSurf->Release();  s_rtSurf = NULL; }
    if (s_rtDepth) { s_rtDepth->Release(); s_rtDepth = NULL; }
    if (s_rt) { s_rt->Release();      s_rt = NULL; }

    /* linear A8R8G8B8 (transparent around the console). Sized to the console. */
    if (FAILED(d->CreateTexture((UINT)w, (UINT)h, 1, D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_rt)))
        return 0;
    if (FAILED(s_rt->GetSurfaceLevel(0, &s_rtSurf))) {
        s_rt->Release(); s_rt = NULL; return 0;
    }
    if (FAILED(d->CreateDepthStencilSurface((UINT)w, (UINT)h,
        D3DFMT_D24S8, D3DMULTISAMPLE_NONE, &s_rtDepth))) {
        s_rtSurf->Release(); s_rtSurf = NULL;
        s_rt->Release(); s_rt = NULL; return 0;
    }
    s_rtW = w; s_rtH = h;
    return 1;
}

/* must be called by the renderer just before CaptureBegin so the RT can be
   sized to the console. vx,vy,vw,vh are the console's flat virtual rect. */
static void ComputeCapRect(float vx, float vy, float vw, float vh) {
    int px = (int)UI_Sx(vx) - CAP_PAD;
    int py = (int)UI_Sy(vy) - CAP_PAD;
    int pw = (int)UI_ScaleX(vw) + CAP_PAD * 2;
    int ph = (int)UI_ScaleY(vh) + CAP_PAD * 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    s_capX = px; s_capY = py; s_capW = pw; s_capH = ph;
}

int Swing_CaptureBegin(float vx, float vy, float vw, float vh) {
    IDirect3DDevice8* d = Gfx_Device();
    if (!s_active || !d) return 0;

    ComputeCapRect(vx, vy, vw, vh);
    /* full-frame RT: the console draws at its normal screen position with no
       viewport offset (negative viewport origins are illegal on the NV2A and
       made the console vanish). We sample the console's sub-rect on the quad. */
    if (!EnsureTargets(d, Gfx_Width(), Gfx_Height())) return 0;

    if (FAILED(d->GetRenderTarget(&s_saveCol))) { return 0; }
    if (FAILED(d->GetDepthStencilSurface(&s_saveZ))) { s_saveCol->Release(); s_saveCol = NULL; return 0; }

    if (FAILED(d->SetRenderTarget(s_rtSurf, s_rtDepth))) {
        s_saveCol->Release(); s_saveCol = NULL;
        s_saveZ->Release();   s_saveZ = NULL;
        return 0;
    }
    d->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0);
    s_capturing = 1;
    return 1;   /* caller now draws the console normally; it lands in the RT */
}

void Swing_CaptureEnd(void) {
    IDirect3DDevice8* d = Gfx_Device();
    if (!s_capturing || !d) return;
    d->SetRenderTarget(s_saveCol, s_saveZ);
    if (s_saveCol) { s_saveCol->Release(); s_saveCol = NULL; }
    if (s_saveZ) { s_saveZ->Release();   s_saveZ = NULL; }
    s_capturing = 0;
}

/* ---------------------------------------------------------------------------
   Draw the captured console as the swinging quad.
   vx,vy,vw,vh = the console's flat virtual rect (must match what was captured).
   The RT now holds ONLY the console (at native density), so we sample the whole
   texture 0..1 -- no sub-rect, no resample blur.
---------------------------------------------------------------------------*/
void Swing_DrawCaptured(float vx, float vy, float vw, float vh) {
    IDirect3DDevice8* d = Gfx_Device();
    D3DVIEWPORT8 vpOld, vp;
    D3DXMATRIX   view, proj, world, rotY, trans;
    D3DXVECTOR3  eye, at, up;
    SwingVert    q[6];
    float        ang, halfW, halfH;

    if (!s_active || !s_rt) return;
    if (!Swing_Doorturning()) return;   /* flat during the rect-fade phases */

    ang = Swing_Angle();

    /* viewport = the padded capture rect on screen (where the console lives) */
    d->GetViewport(&vpOld);
    vp.X = (DWORD)s_capX;
    vp.Y = (DWORD)s_capY;
    vp.Width = (DWORD)s_capW;
    vp.Height = (DWORD)s_capH;
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    d->SetViewport(&vp);

    eye.x = 0.0f; eye.y = 0.0f; eye.z = -3.0f;
    at.x = 0.0f; at.y = 0.0f; at.z = 0.0f;
    up.x = 0.0f; up.y = 1.0f; up.z = 0.0f;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, 0.9f,
        (float)vp.Width / (float)vp.Height, 1.0f, 100.0f);
    d->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
    d->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);

    /* hinge on the RIGHT edge */
    halfW = 1.0f; halfH = 1.30f;
    D3DXMatrixRotationY(&rotY, ang);
    D3DXMatrixTranslation(&trans, -halfW, 0.0f, 0.0f);
    D3DXMatrixMultiply(&world, &trans, &rotY);
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);

    /* sample the capture rect's sub-region of the full-frame RT (0..1 UVs) */
    {
        float u0 = (float)s_capX / (float)s_rtW;
        float v0 = (float)s_capY / (float)s_rtH;
        float u1 = (float)(s_capX + s_capW) / (float)s_rtW;
        float v1 = (float)(s_capY + s_capH) / (float)s_rtH;
        float x0 = 0.0f, x1 = 2.0f * halfW;
        float y0 = -halfH, y1 = halfH;
        q[0].x = x0; q[0].y = y1; q[0].z = 0; q[0].u = u0; q[0].v = v0;
        q[1].x = x1; q[1].y = y1; q[1].z = 0; q[1].u = u1; q[1].v = v0;
        q[2].x = x0; q[2].y = y0; q[2].z = 0; q[2].u = u0; q[2].v = v1;
        q[3].x = x1; q[3].y = y1; q[3].z = 0; q[3].u = u1; q[3].v = v0;
        q[4].x = x1; q[4].y = y0; q[4].z = 0; q[4].u = u1; q[4].v = v1;
        q[5].x = x0; q[5].y = y0; q[5].z = 0; q[5].u = u0; q[5].v = v1;
    }

    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTexture(0, s_rt);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    /* linear so the panel stays smooth while it's rotated/foreshortened; with
       the 1:1 native-density capture there's no scale-down blur for it to
       amplify, so the result is crisp at rest and clean in motion. */
    d->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    d->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    d->SetVertexShader(SWING_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(SwingVert));

    /* restore */
    d->SetTexture(0, NULL);
    D3DXMatrixIdentity(&world);
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&world);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetViewport(&vpOld);
}