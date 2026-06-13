/*---------------------------------------------------------------------------
    dd_plasma -- the "flubber": a lit, glossy, organic liquid mass on the
    pedestal, in the spirit of the original Xbox boot blob.

    - High-poly UV sphere, deformed by several slowly travelling lumps plus an
      overall breathe + pulse, so it reads as a morphing liquid mass.
    - Every so often one lump spikes hard and a globule "tries to escape"
      (shoots out and retracts).
    - Vertex normals are recomputed each frame from the deformed surface; the
      mass is lit (directional + specular) so the form and wet sheen read.
    - Per-vertex colour comes from an animated plasma field tinted between a
      theme BASE colour and a hue-derived ACCENT, so the colour fluctuates and
      flows across the surface while staying lit.
    - A few additive arcs drift around it.
    - The pedestal light shaft is drawn beneath it.

    Same sub-viewport / camera scheme as dd_pedestal (DrawPrimitiveUP). State
    is restored to 2D-friendly defaults afterwards. Toggled by DD_FX_PLASMA.
---------------------------------------------------------------------------*/
#include "dd_plasma.h"
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "dd_audio.h"
#include <d3d8.h>
#include <d3dx8.h>
#include <math.h>
#include <string.h>

/* ---- tuning ------------------------------------------------------------- */
#define SPH_STACKS   28          /* half tessellation: paired with the resident VB for extra */
#define SPH_SLICES   40          /* load margin. ~200px orb looks identical; halves GPU + CPU + copy. */
#define SPH_ROWS     (SPH_STACKS + 1)
#define SPH_GRID     (SPH_ROWS * SPH_SLICES)
#define SPH_TRIS     (SPH_STACKS * SPH_SLICES * 2)
#define SPH_VERTS    (SPH_TRIS * 3)

#define PL_RADIUS    0.85f
#define PL_MAXEXT    1.45f      /* hard cap on a vertex's distance from centre (anti-clip) */
#define PL_LIFT      0.18f      /* raise the blob so it floats above the beam   */
#define PL_PULSE     0.06f
#define PL_BREATHE   0.06f
#define PL_LUMP      0.24f
#define PL_TILT      (-0.40f)
#define PL_SPECPOW   20.0f

#define PL_REACT_PULSE 0.30f    /* bass swell at full level          */
#define PL_REACT_LUMP  1.90f    /* bass lumpiness at full level      */
#define PL_REACT_SMOOTH 0.45f   /* body (bass) responsiveness         */
#define PL_REACT_SMOOTH_HI 0.65f /* spark (treble) responsiveness -- snappier */
#define PL_ALPHA       100      /* blob opacity 0..255 (lower = more see-through) */

#define PL_ESC_PERIOD 11000u    /* ms between "escape" attempts          */
#define PL_ESC_DUR     1500u    /* ms the globule is out                 */
#define PL_ESC_AMP     0.78f    /* how far it reaches                    */

#define PL_ARCS        3
#define PL_ARCSEG      14
#define PL_ARC_RETARGET 900u

#define PL_TABLE     1024
#define PL_MASK      (PL_TABLE - 1)

#define SPH_FVF  (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE)
#define ARC_FVF  (D3DFVF_XYZ | D3DFVF_DIFFUSE)
typedef struct { float x, y, z, nx, ny, nz; DWORD c; } NVert;
typedef struct { float x, y, z; DWORD c; }             AVert;

static int   s_ready = 0;
static int   s_sin[PL_TABLE];
static float s_dx[SPH_GRID], s_dy[SPH_GRID], s_dz[SPH_GRID];
static float s_px[SPH_GRID], s_py[SPH_GRID], s_pz[SPH_GRID];
static float s_nx[SPH_GRID], s_ny[SPH_GRID], s_nz[SPH_GRID];
static DWORD s_col[SPH_GRID];
static NVert s_vtx[SPH_VERTS];

/* Resident dynamic vertex buffer for the sphere, replacing the per-frame 2x
   DrawPrimitiveUP. The UP path copied ~735KB/frame through D3D's shared scratch
   ring; when that ring wrapped against an in-flight GPU read it corrupted the
   pushbuffer and hard-wedged the NV2A (which holds the memory bus and freezes
   the whole CPU). A dedicated VB removes that entirely: fill once per frame,
   draw both hemispheres from it. Double-buffered (ping-pong) because Xbox Lock
   does NOT stall on a buffer the GPU is still reading -- we write the one the
   GPU finished two frames ago, never the one in flight. */
static IDirect3DVertexBuffer8* s_vb[2] = { NULL, NULL };
static int   s_vbState = 0;     /* 0 = not yet created, 1 = ready, -1 = create failed (UP fallback) */
static int   s_vbFrame = 0;     /* ping-pong index */

static unsigned char s_pal[256][3];
static DWORD s_palBase = 0xFFFFFFFF;
static int   s_palValid = 0;

static int s_arcA[PL_ARCS], s_arcB[PL_ARCS];
static DWORD s_arcNext = 0;
static unsigned long s_rng = 0x51ed2719UL;
static unsigned long prng(void) { s_rng = s_rng * 1664525UL + 1013904223UL; return s_rng; }

static int gidx(int i, int j) { return i * SPH_SLICES + j; }
static void norm3(float* x, float* y, float* z) {
    float l = (float)sqrt((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
    if (l > 0.0001f) { *x /= l; *y /= l; *z /= l; }
}

static void plasma_init(void) {
    int i, j, n;
    if (s_ready) return;
    s_ready = 1;
    for (i = 0; i < PL_TABLE; i++)
        s_sin[i] = (int)(sin((double)i * 6.283185307 / (double)PL_TABLE) * 255.0);
    for (i = 0; i <= SPH_STACKS; i++) {
        double lat = 3.141592653 * (double)i / (double)SPH_STACKS;
        float cy = (float)cos(lat), sr = (float)sin(lat);
        for (j = 0; j < SPH_SLICES; j++) {
            double lon = 6.283185307 * (double)j / (double)SPH_SLICES;
            n = gidx(i, j);
            s_dx[n] = sr * (float)cos(lon); s_dy[n] = cy; s_dz[n] = sr * (float)sin(lon);
        }
    }
    for (i = 0; i < PL_ARCS; i++) { s_arcA[i] = 0; s_arcB[i] = 0; }
}

/* dark -> base -> accent -> white-hot; accent is hue-rotated from base */
static void rgb2hsv(int r, int g, int b, float* h, float* s, float* v) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn;
    *v = (float)mx / 255.0f; *s = mx ? (float)d / (float)mx : 0.0f;
    if (!d) { *h = 0.0f; return; }
    if (mx == r) *h = 60.0f * ((float)(g - b) / (float)d);
    else if (mx == g) *h = 60.0f * (2.0f + (float)(b - r) / (float)d);
    else *h = 60.0f * (4.0f + (float)(r - g) / (float)d);
    if (*h < 0.0f) *h += 360.0f;
}
static DWORD hsv2rgb(float h, float s, float v) {
    float c = v * s, x, m = v - c; int r, g, b, hi;
    while (h < 0.0f) h += 360.0f; while (h >= 360.0f) h -= 360.0f;
    hi = (int)(h / 60.0f);
    x = c * (1.0f - (float)fabs(fmod(h / 60.0f, 2.0f) - 1.0f));
    switch (hi) {
    case 0: r = (int)((c + m) * 255); g = (int)((x + m) * 255); b = (int)(m * 255); break;
    case 1: r = (int)((x + m) * 255); g = (int)((c + m) * 255); b = (int)(m * 255); break;
    case 2: r = (int)(m * 255); g = (int)((c + m) * 255); b = (int)((x + m) * 255); break;
    case 3: r = (int)(m * 255); g = (int)((x + m) * 255); b = (int)((c + m) * 255); break;
    case 4: r = (int)((x + m) * 255); g = (int)(m * 255); b = (int)((c + m) * 255); break;
    default:r = (int)((c + m) * 255); g = (int)(m * 255); b = (int)((x + m) * 255); break;
    }
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    return ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
}
/* clamp + pack helpers */
static DWORD clampcol(int r, int g, int b) {
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    return ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
}
static DWORD lerpcol(DWORD a, DWORD c, int t /*0..255*/) {
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
    return clampcol(ar + (cr - ar) * t / 255, ag + (cg - ag) * t / 255, ab + (cb - ab) * t / 255);
}

/* Build a harmonised ramp from the primary colour: dark -> base -> analogous
   -> (split) complementary -> white-hot. Multiple complementary hues, all
   derived from the one theme colour, so the surface shows a cohesive spread. */
static DWORD s_accent = 0xFFFFFFFF;
static void plasma_palette(DWORD base) {
    int k, br, bg, bb; float h, s, v;
    DWORD kDark, kBase, kAnalog, kComp, kWhite, key[5];
    if (s_palValid && base == s_palBase) return;
    s_palBase = base; s_palValid = 1;

    br = (int)((base >> 16) & 0xFF); bg = (int)((base >> 8) & 0xFF); bb = (int)(base & 0xFF);
    rgb2hsv(br, bg, bb, &h, &s, &v);
    if (s < 0.35f) s = 0.35f;                 /* keep washed-out themes colourful */

    kDark = hsv2rgb(h, s * 0.92f, v * 0.45f);                       /* gentle low end          */
    kBase = hsv2rgb(h, s, v < 0.88f ? v + 0.06f : v);
    kAnalog = hsv2rgb(h + 16.0f, s * 0.96f, v < 0.85f ? v + 0.10f : 0.95f);   /* subtle warm shift       */
    kComp = hsv2rgb(h + 30.0f, s * 0.90f, v < 0.82f ? v + 0.16f : 0.96f);   /* still in the same family */
    kWhite = hsv2rgb(h + 10.0f, s * 0.45f, 0.98f);                            /* soft tinted highlight    */
    s_accent = hsv2rgb(h + 24.0f, s * 0.95f + 0.05f, 0.95f);                     /* arcs: bright, in-family  */

    key[0] = kDark; key[1] = kBase; key[2] = kAnalog; key[3] = kComp; key[4] = kWhite;
    for (k = 0; k < 256; k++) {
        int seg = k * 4 / 256;                /* 0..3 */
        int t = (k * 4) - seg * 256;          /* 0..255 within segment */
        DWORD c = lerpcol(key[seg], key[seg + 1], t);
        s_pal[k][0] = (unsigned char)((c >> 16) & 0xFF);
        s_pal[k][1] = (unsigned char)((c >> 8) & 0xFF);
        s_pal[k][2] = (unsigned char)(c & 0xFF);
    }
}

void Plasma_Draw(float vx, float vy, float vw, float vh, DWORD accent, DWORD glow) {
    IDirect3DDevice8* d = Gfx_Device();
    D3DVIEWPORT8 vpOld, vp;
    D3DXMATRIX   view, proj, ident;
    D3DXVECTOR3  eye, at, up;
    D3DLIGHT8    light; D3DMATERIAL8 mat; AVert beam[6];
    float t, cosA, sinA, cosT, sinT, scale, breathe, lump, lf, hf;
    float l0x, l0y, l0z, l1x, l1y, l1z, l2x, l2y, l2z;
    float ex = 0, ey = 0, ez = 0, escAmp = 0, br, bg, bb;
    int   n, i, j, v, apexA, t1, t2, t3; DWORD ms, apexC, topC, ecycle, ephase;

    (void)glow;
    if (!d) return;
    plasma_init(); plasma_palette(accent);

    ms = GetTickCount(); t = (float)ms;
    t1 = (int)(ms / 22); t2 = (int)(ms / 31); t3 = (int)(ms / 41);
    cosA = (float)cos(t * 0.00038f); sinA = (float)sin(t * 0.00038f);
    cosT = (float)cos(PL_TILT);      sinT = (float)sin(PL_TILT);
    scale = PL_RADIUS * (1.0f + PL_PULSE * (float)sin(t * 0.0016f));
    breathe = PL_BREATHE * (float)sin(t * 0.0011f);

    /* music reactivity, split by band: BASS swells/lumps the body, TREBLE feeds
       the sparks (arcs). Both ease back to the calm baseline when quiet. */
    {
        int lo = 0, hi = 0;
        static float loS = 0.0f, hiS = 0.0f;
        Audio_MusicLevels(&lo, &hi);
        loS += ((float)lo / 255.0f - loS) * PL_REACT_SMOOTH;
        hiS += ((float)hi / 255.0f - hiS) * PL_REACT_SMOOTH_HI;
        lf = loS; hf = hiS;
    }
    scale *= (1.0f + PL_REACT_PULSE * lf);
    lump = PL_LUMP * (1.0f + PL_REACT_LUMP * lf);

    l0x = (float)sin(t * 0.00060f); l0y = (float)cos(t * 0.00048f); l0z = (float)sin(t * 0.00072f); norm3(&l0x, &l0y, &l0z);
    l1x = (float)cos(t * 0.00052f); l1y = (float)sin(t * 0.00067f); l1z = (float)cos(t * 0.00041f); norm3(&l1x, &l1y, &l1z);
    l2x = (float)sin(t * 0.00058f + 2.0f); l2y = (float)sin(t * 0.00039f + 1.0f); l2z = (float)cos(t * 0.00063f); norm3(&l2x, &l2y, &l2z);

    /* "escape" globule: once per period a narrow finger reaches out then retracts */
    ecycle = ms / PL_ESC_PERIOD;
    ephase = ms - ecycle * PL_ESC_PERIOD;
    if (ephase < PL_ESC_DUR) {
        float p = (float)ephase / (float)PL_ESC_DUR;       /* 0..1   */
        escAmp = PL_ESC_AMP * (float)sin(3.14159265f * p);  /* 0->1->0 */
        s_rng = ecycle * 2654435761UL + 12345UL;            /* stable dir for this event */
        ex = (float)(int)((prng() >> 9) & 0x1FF) - 256.0f;
        ey = (float)(int)((prng() >> 9) & 0x1FF) - 256.0f;
        ez = (float)(int)((prng() >> 9) & 0x1FF) - 256.0f;
        norm3(&ex, &ey, &ez);
    }

    /* ---- pose grid: lumps + escape; colour from the animated plasma field ---- */
    for (n = 0; n < SPH_GRID; n++) {
        float dx = s_dx[n], dy = s_dy[n], dz = s_dz[n];
        float d0 = dx * l0x + dy * l0y + dz * l0z;
        float d1 = dx * l1x + dy * l1y + dz * l1z;
        float d2 = dx * l2x + dy * l2y + dz * l2z;
        float r = 1.0f + breathe, x1, y1, z1; int ii = n / SPH_SLICES, jj = n % SPH_SLICES, pv, ci;
        if (d0 > 0.0f) r += lump * d0 * d0 * d0 * d0;
        if (d1 > 0.0f) r += lump * d1 * d1 * d1 * d1;
        if (d2 > 0.0f) r += lump * d2 * d2 * d2 * d2;
        if (escAmp > 0.0f) {
            float de = dx * ex + dy * ey + dz * ez;
            if (de > 0.0f) { float d4 = de * de; d4 = d4 * d4; r += escAmp * d4 * d4; }  /* de^8: narrow finger */
        }
        x1 = (dx * r) * cosA + (dz * r) * sinA; z1 = -(dx * r) * sinA + (dz * r) * cosA; y1 = dy * r;
        {
            float px = x1 * scale;
            float py = (y1 * cosT - z1 * sinT) * scale;
            float pz = (y1 * sinT + z1 * cosT) * scale;
            float m2 = px * px + py * py + pz * pz;
            if (m2 > PL_MAXEXT * PL_MAXEXT) {       /* rein in lumps/escape/swell so nothing clips */
                float k = PL_MAXEXT / (float)sqrt(m2);
                px *= k; py *= k; pz *= k;
            }
            s_px[n] = px; s_py[n] = py + PL_LIFT; s_pz[n] = pz;
        }

        pv = s_sin[(ii * 19 + t1) & PL_MASK] + s_sin[(jj * 26 + t2) & PL_MASK]
            + s_sin[((ii + jj) * 11 + t3) & PL_MASK];
        ci = (pv + 765) * 255 / 1530; if (ci < 0) ci = 0; else if (ci > 255) ci = 255;
        s_col[n] = ((DWORD)PL_ALPHA << 24) | ((DWORD)s_pal[ci][0] << 16) | ((DWORD)s_pal[ci][1] << 8) | (DWORD)s_pal[ci][2];
    }

    /* ---- recompute smooth normals from the deformed surface ---- */
    for (i = 0; i <= SPH_STACKS; i++) {
        int ip = (i < SPH_STACKS) ? i + 1 : i, im = (i > 0) ? i - 1 : i;
        for (j = 0; j < SPH_SLICES; j++) {
            int jp = (j + 1) % SPH_SLICES, jm = (j + SPH_SLICES - 1) % SPH_SLICES, a = gidx(i, j);
            float tux = s_px[gidx(i, jp)] - s_px[gidx(i, jm)];
            float tuy = s_py[gidx(i, jp)] - s_py[gidx(i, jm)];
            float tuz = s_pz[gidx(i, jp)] - s_pz[gidx(i, jm)];
            float tvx = s_px[gidx(ip, j)] - s_px[gidx(im, j)];
            float tvy = s_py[gidx(ip, j)] - s_py[gidx(im, j)];
            float tvz = s_pz[gidx(ip, j)] - s_pz[gidx(im, j)];
            float nx = tvy * tuz - tvz * tuy, ny = tvz * tux - tvx * tuz, nz = tvx * tuy - tvy * tux;
            float ox = s_px[a], oy = s_py[a], oz = s_pz[a];
            if (nx * nx + ny * ny + nz * nz < 0.000001f) { nx = ox; ny = oy; nz = oz; }
            norm3(&nx, &ny, &nz);
            if (nx * ox + ny * oy + nz * oz < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
            s_nx[a] = nx; s_ny[a] = ny; s_nz[a] = nz;
        }
    }

    /* ---- assemble triangle list ---- */
    v = 0;
    for (i = 0; i < SPH_STACKS; i++) {
        for (j = 0; j < SPH_SLICES; j++) {
            int jp = (j + 1) % SPH_SLICES;
            int q[6], k; q[0] = gidx(i, j); q[1] = gidx(i + 1, j); q[2] = gidx(i, jp);
            q[3] = gidx(i, jp); q[4] = gidx(i + 1, j); q[5] = gidx(i + 1, jp);
            for (k = 0; k < 6; k++) {
                int g = q[k];
                s_vtx[v].x = s_px[g]; s_vtx[v].y = s_py[g]; s_vtx[v].z = s_pz[g];
                s_vtx[v].nx = s_nx[g]; s_vtx[v].ny = s_ny[g]; s_vtx[v].nz = s_nz[g];
                s_vtx[v].c = s_col[g];
                v++;
            }
        }
    }

    /* ---- the pedestal: themed platform sprite (same placement the launcher and
       egg use), drawn in 2D BEFORE switching to the 3D sub-viewport ---- */
    {
        const Texture* basep = Theme_Asset("platform_round");
        if (basep) UI_DrawSprite(basep, 70.0f, 270.0f, 220.0f, 116.0f, 0xFFFFFFFF, 0);
    }
    (void)vx; (void)vy; (void)vw; (void)vh;   /* blob uses the pedestal's placement */

    /* ---- viewport + camera (the launcher/egg pedestal placement) ---- */
    d->GetViewport(&vpOld);
    vp.X = (DWORD)UI_Sx(80.0f); vp.Y = (DWORD)UI_Sy(95.0f);
    vp.Width = (DWORD)UI_ScaleX(200.0f); vp.Height = (DWORD)UI_ScaleY(220.0f);
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f; d->SetViewport(&vp);
    eye.x = 0; eye.y = 0; eye.z = -4.2f; at.x = at.y = at.z = 0; up.x = 0; up.y = 1; up.z = 0;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, 0.8f, (float)vp.Width / (float)vp.Height, 1.0f, 100.0f);
    D3DXMatrixIdentity(&ident);
    d->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
    d->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);
    d->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&ident);

    br = (float)((accent >> 16) & 0xFF) / 255.0f; bg = (float)((accent >> 8) & 0xFF) / 255.0f; bb = (float)(accent & 0xFF) / 255.0f;

    /* ---- pedestal light shaft (additive, unlit) ---- */
    apexA = 150 + (int)((ms >> 4) & 63);
    apexC = ((DWORD)apexA << 24) | (accent & 0x00FFFFFF); topC = (accent & 0x00FFFFFF);
    beam[0].x = 0; beam[0].y = -1.85f; beam[0].z = 0; beam[0].c = apexC;
    beam[1].x = -0.9f; beam[1].y = -0.95f; beam[1].z = 0; beam[1].c = topC;
    beam[2].x = 0.9f; beam[2].y = -0.95f; beam[2].z = 0; beam[2].c = topC;
    beam[3].x = 0; beam[3].y = -1.85f; beam[3].z = 0; beam[3].c = apexC;
    beam[4].x = 0; beam[4].y = -0.95f; beam[4].z = -0.9f; beam[4].c = topC;
    beam[5].x = 0; beam[5].y = -0.95f; beam[5].z = 0.9f; beam[5].c = topC;
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    d->SetTexture(0, NULL);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(ARC_FVF);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, beam, sizeof(AVert));

    /* ---- the lit flubber: vertex colour feeds diffuse, material gives the sheen ---- */
    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
    light.Specular.r = light.Specular.g = light.Specular.b = 1.0f;
    light.Direction.x = 0.40f; light.Direction.y = -0.55f; light.Direction.z = 0.73f;
    d->SetLight(0, &light); d->LightEnable(0, TRUE);

    memset(&mat, 0, sizeof(mat));
    mat.Specular.r = mat.Specular.g = mat.Specular.b = 1.0f;
    mat.Emissive.r = br * 0.14f; mat.Emissive.g = bg * 0.14f; mat.Emissive.b = bb * 0.14f;
    mat.Power = PL_SPECPOW;
    d->SetMaterial(&mat);

    d->SetRenderState(D3DRS_AMBIENT, 0x00303030);
    d->SetRenderState(D3DRS_LIGHTING, TRUE);
    d->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
    d->SetRenderState(D3DRS_LOCALVIEWER, TRUE);
    d->SetRenderState(D3DRS_COLORVERTEX, TRUE);
    d->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
    d->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1);
    d->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    d->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    /* semi-transparent: blend over the beam/backdrop. Drawn as two hemispheres
       (back then front) via the two cull modes so the layers composite in order
       without z-sort artifacts, and it can never cull to nothing. */
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d->SetVertexShader(SPH_FVF);

    /* one-time: create the ping-pong dynamic VBs. Managed pool = always lockable
       on Xbox with no host-copy sync (unified memory). */
    if (s_vbState == 0) {
        int i; s_vbState = 1;
        for (i = 0; i < 2; i++) {
            if (FAILED(d->CreateVertexBuffer(SPH_VERTS * sizeof(NVert),
                D3DUSAGE_WRITEONLY, SPH_FVF, D3DPOOL_MANAGED, &s_vb[i])) || !s_vb[i]) {
                s_vbState = -1; break;
            }
        }
    }

    if (s_vbState == 1) {
        IDirect3DVertexBuffer8* vb = s_vb[s_vbFrame & 1];
        BYTE* pv = NULL;
        s_vbFrame++;
        if (SUCCEEDED(vb->Lock(0, SPH_VERTS * sizeof(NVert), &pv, 0)) && pv) {
            memcpy(pv, s_vtx, SPH_VERTS * sizeof(NVert));
            vb->Unlock();
        }
        d->SetStreamSource(0, vb, sizeof(NVert));
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);   /* far hemisphere first */
        d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, SPH_TRIS);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);  /* near hemisphere over it */
        d->DrawPrimitive(D3DPT_TRIANGLELIST, 0, SPH_TRIS);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    }
    else {
        /* VB creation failed -- fall back to the original UP path so the orb
           still renders (with the old hazard, but at least it draws). */
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, SPH_TRIS, s_vtx, sizeof(NVert));
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
        d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, SPH_TRIS, s_vtx, sizeof(NVert));
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    }

    /* ---- drifting arcs (additive, unlit, on top) ---- */
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORVERTEX, FALSE);
    d->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    d->SetVertexShader(ARC_FVF);
    if (ms >= s_arcNext) {                  /* drift one endpoint at a time, gently */
        int g2;
        s_arcNext = ms + PL_ARC_RETARGET;
        for (g2 = 0; g2 < PL_ARCS; g2++) {
            s_arcA[g2] = s_arcB[g2] ? s_arcB[g2] : (int)(prng() % (unsigned long)SPH_GRID);
            s_arcB[g2] = (int)(prng() % (unsigned long)SPH_GRID);
        }
    }
    {
        int gr = (int)((s_accent >> 16) & 0xFF), gg = (int)((s_accent >> 8) & 0xFF), gb = (int)(s_accent & 0xFF);
        float ab = 0.18f + 1.10f * hf;          /* treble sparks the arcs */
        DWORD hot = 0xFF000000u
            | ((DWORD)(int)(((gr + 255) / 2) * ab) << 16)
            | ((DWORD)(int)(((gg + 255) / 2) * ab) << 8)
            | (DWORD)(int)(((gb + 255) / 2) * ab);
        int g2, k, pass;
        for (g2 = 0; g2 < PL_ARCS; g2++) {
            int a = s_arcA[g2], b = s_arcB[g2];
            for (pass = 0; pass < 2; pass++) {          /* two slightly offset passes = thicker glow */
                AVert arc[PL_ARCSEG + 1];
                float ox = (pass ? 0.02f : -0.02f);
                for (k = 0; k <= PL_ARCSEG; k++) {
                    float f = (float)k / (float)PL_ARCSEG;
                    float bulge = 1.0f + 0.20f * (float)sin(3.14159265f * f);
                    float wob = (0.03f + 0.18f * hf) * (float)sin(8.0f * f + t * 0.004f);
                    arc[k].x = (s_px[a] + (s_px[b] - s_px[a]) * f) * bulge + ox;
                    arc[k].y = (s_py[a] + (s_py[b] - s_py[a]) * f) * bulge + wob;
                    arc[k].z = (s_pz[a] + (s_pz[b] - s_pz[a]) * f) * bulge;
                    arc[k].c = hot;
                }
                d->DrawPrimitiveUP(D3DPT_LINESTRIP, PL_ARCSEG, arc, sizeof(AVert));
            }
        }
    }

    /* ---- restore 2D-friendly state ---- */
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetViewport(&vpOld);
}

void Plasma_Release(void) {
    s_ready = 0; s_palValid = 0;
    if (s_vb[0]) { s_vb[0]->Release(); s_vb[0] = NULL; }
    if (s_vb[1]) { s_vb[1]->Release(); s_vb[1] = NULL; }
    s_vbState = 0; s_vbFrame = 0;
}