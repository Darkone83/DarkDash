/*---------------------------------------------------------------------------
    dd_fx.cpp -- ambient character overlays (see dd_fx.h).

    Scanline texture: a small A8R8G8B8 texture, black on odd rows / clear on
    even, drawn full-screen with normal alpha to darken alternate lines. A slow
    vertical roll band rides on top. Edge glow + boot intro are procedural.

    Build: MSVC2003/C89 style; file-scope statics; dd_ftol supplies the float
    helper so (int) casts are fine.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <math.h>
#include "dd_fx.h"
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include <XGraphics.h>

#define SCAN_TEX_H 256          /* scanline texture height (pow2)            */
#define SCAN_TEX_W   4

static Texture s_scan = { 0, 0, 0, 0, 0 };
static int     s_ok = 0;

static DWORD   s_edgeAt = 0;    /* tick the edge flash was triggered (0=none) */
#define EDGE_MS 360.0f

static DWORD   s_bootAt = 0;    /* tick the boot intro began                 */
static int     s_bootDone = 1;
#define BOOT_MS 1100.0f

/* ---- init -------------------------------------------------------------- */

int Fx_Init(void) {
    int x, y;
    unsigned char* buf;
    IDirect3DTexture8* tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;

    s_scan.tex = NULL; s_ok = 0;

    buf = (unsigned char*)malloc((size_t)SCAN_TEX_W * SCAN_TEX_H * 4);
    if (!buf) return 0;

    /* odd rows: semi-opaque black (the dark scanline); even rows: clear */
    for (y = 0; y < SCAN_TEX_H; y++) {
        int dark = (y & 1) ? 90 : 0;     /* alpha of the dark line */
        for (x = 0; x < SCAN_TEX_W; x++) {
            unsigned char* p = buf + ((size_t)y * SCAN_TEX_W + x) * 4;
            p[0] = 0; p[1] = 0; p[2] = 0; p[3] = (unsigned char)dark;
        }
    }

    hr = Gfx_Device()->CreateTexture(SCAN_TEX_W, SCAN_TEX_H, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
    if (FAILED(hr)) { free(buf); return 0; }
    hr = tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr)) { tex->Release(); free(buf); return 0; }
    XGSwizzleRect(buf, SCAN_TEX_W * 4, NULL, lr.pBits, SCAN_TEX_W, SCAN_TEX_H, NULL, 4);
    tex->UnlockRect(0);
    free(buf);

    s_scan.tex = tex;
    s_scan.w = SCAN_TEX_W; s_scan.h = SCAN_TEX_H;
    s_scan.pw = SCAN_TEX_W; s_scan.ph = SCAN_TEX_H;
    s_ok = 1;
    return 1;
}

void Fx_Shutdown(void) {
    if (s_scan.tex) Texture_Release(&s_scan);
    s_scan.tex = NULL; s_ok = 0;
}

/* ---- CRT scanlines + roll --------------------------------------------- */

void Fx_DrawScanlines(void) {
    DWORD t;
    float roll;
    if (!s_ok || !s_scan.tex) return;

    /* scanlines: stretch the 256-tall texture over the 480 virtual height.
       Drawn at modest alpha so it textures the image without crushing it. */
    UI_DrawSprite(&s_scan, 0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(120, 255, 255, 255), 0);

    /* slow vertical roll: a faint bright band easing down the screen ~ every
       9s, like a CRT refresh sweep. Additive, very subtle. */
    t = GetTickCount();
    roll = (float)(t % 9000) / 9000.0f;           /* 0..1 top->bottom */
    {
        float bandH = 70.0f;
        float by = roll * (480.0f + bandH) - bandH;
        UI_FillRectAdd(0.0f, by, 640.0f, bandH, UI_ARGB(14, 255, 255, 255));
    }
}

/* ---- edge-glow flash --------------------------------------------------- */

void Fx_FlashEdge(void) { s_edgeAt = GetTickCount(); }

void Fx_DrawEdgeGlow(void) {
    DWORD glow, t;
    float p, hump;
    int   gr, gg, gb, a, thick;
    if (s_edgeAt == 0) return;
    t = GetTickCount();
    if (t - s_edgeAt >= (DWORD)EDGE_MS) { s_edgeAt = 0; return; }

    glow = Theme_Color("glow", 0xFFAEFF3C);
    gr = (int)((glow >> 16) & 0xFF); gg = (int)((glow >> 8) & 0xFF); gb = (int)(glow & 0xFF);

    p = (float)(t - s_edgeAt) / EDGE_MS;           /* 0..1 */
    hump = 4.0f * p * (1.0f - p);                  /* 0->1->0 */
    a = (int)(150.0f * hump);
    if (a <= 0) return;
    thick = 6;

    /* four additive border bars pulsing the screen edge */
    UI_FillRectAdd(0.0f, 0.0f, 640.0f, (float)thick, UI_ARGB(a, gr, gg, gb)); /* top */
    UI_FillRectAdd(0.0f, 480.0f - (float)thick, 640.0f, (float)thick, UI_ARGB(a, gr, gg, gb)); /* bottom */
    UI_FillRectAdd(0.0f, 0.0f, (float)thick, 480.0f, UI_ARGB(a, gr, gg, gb)); /* left */
    UI_FillRectAdd(640.0f - (float)thick, 0.0f, (float)thick, 480.0f, UI_ARGB(a, gr, gg, gb)); /* right */
}

/* ---- boot intro -------------------------------------------------------- */

void Fx_BootBegin(void) { s_bootAt = GetTickCount(); s_bootDone = 0; }

int Fx_BootActive(void) {
    if (s_bootDone) return 0;
    if (GetTickCount() - s_bootAt >= (DWORD)BOOT_MS) { s_bootDone = 1; return 0; }
    return 1;
}

void Fx_DrawBoot(void) {
    DWORD glow, t;
    float p;
    int   gr, gg, gb;
    if (s_bootDone) return;
    t = GetTickCount();
    p = (float)(t - s_bootAt) / BOOT_MS;           /* 0..1 */
    if (p >= 1.0f) { s_bootDone = 1; return; }

    glow = Theme_Color("glow", 0xFFAEFF3C);
    gr = (int)((glow >> 16) & 0xFF); gg = (int)((glow >> 8) & 0xFF); gb = (int)(glow & 0xFF);

    /* phase 1 (0..0.45): a bright sweep wipes down revealing the screen,
       fading out as it passes. phase 2 (0.45..1): a quick full-screen flash
       settle. Both additive over the already-drawn content. */
    if (p < 0.45f) {
        float q = p / 0.45f;                       /* 0..1 */
        float by = q * 480.0f;
        float bandH = 90.0f;
        int   a = 180;
        /* leading bright band */
        UI_FillRectAdd(0.0f, by - bandH, 640.0f, bandH, UI_ARGB(a, gr, gg, gb));
        /* darken everything below the sweep (not yet "powered on") */
        UI_FillRect(0.0f, by, 640.0f, 480.0f - by, UI_ARGB(220, 0, 0, 0));
    }
    else {
        float q = (p - 0.45f) / 0.55f;              /* 0..1 */
        int   a = (int)(120.0f * (1.0f - q));        /* fade the wash out */
        if (a > 0) UI_FillRectAdd(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(a, gr, gg, gb));
    }
}