/*---------------------------------------------------------------------------
    dd_screensaver.cpp -- see dd_screensaver.h.

    State machine per title: FADE_IN -> HOLD -> FADE_OUT -> pick next.
    The whole pedestal drifts on a slow Lissajous path (burn-in safe). The light
    shaft hue advances continuously so it sweeps an RGB rainbow.

    C89 style: declarations before statements, file-scope statics, no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_screensaver.h"
#include "dd_launcher.h"
#include "dd_pedestal.h"
#include "dd_texture.h"
#include "dd_gfx.h"
#include "dd_theme.h"
#include "dd_ui.h"
#include "Applications.h"
#include "Games.h"
#include "Homebrew.h"
#include "Emulators.h"

#define SAVER_MAX_POOL   256
#define SAVER_PATH_MAX   256

/* fade timings (ms) */
#define SAVER_FADE_MS    1200u
#define SAVER_HOLD_MS    5000u

enum { SP_FADE_IN = 0, SP_HOLD, SP_FADE_OUT };

static int   s_scanned = 0;
static char  s_pool[SAVER_MAX_POOL][SAVER_PATH_MAX];
static int   s_poolCount = 0;

static Texture s_art;
static int     s_artFlat = 0;
static int     s_haveArt = 0;
static int     s_cur = -1;        /* current pool index */
static unsigned s_rng = 0x1234567u;

static int    s_phase = SP_FADE_IN;
static DWORD  s_phaseStart = 0;
static DWORD  s_enterMs = 0;

/* ---- tiny helpers ------------------------------------------------------ */

static int SvLen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void SvCopy(char* dst, int cap, const char* src) {
    int i = 0; if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void SvJoin(char* out, int cap, const char* base, const char* leaf) {
    int n; SvCopy(out, cap, base); n = SvLen(out);
    if (n > 0 && out[n - 1] != '\\' && n < cap - 1) { out[n++] = '\\'; out[n] = 0; }
    { int i = 0; while (leaf && leaf[i] && n < cap - 1) out[n++] = leaf[i++]; out[n] = 0; }
}

static unsigned SvRand(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

/* HSV (h 0..1535 = 6*256 sextants) -> RGB, full sat/val. Cheap integer ramp. */
static void SvHueRgb(unsigned h, int* r, int* g, int* b) {
    unsigned seg = (h / 256u) % 6u;
    int f = (int)(h % 256u);          /* 0..255 ramp within the segment */
    int up = f, dn = 255 - f;
    switch (seg) {
    case 0: *r = 255; *g = up;  *b = 0;   break;
    case 1: *r = dn;  *g = 255; *b = 0;   break;
    case 2: *r = 0;   *g = 255; *b = up;  break;
    case 3: *r = 0;   *g = dn;  *b = 255; break;
    case 4: *r = up;  *g = 0;   *b = 255; break;
    default:*r = 255; *g = 0;   *b = dn;  break;
    }
}

/* ---- pool scan --------------------------------------------------------- */

static void ScanRootInto(const char* root) {
    char            pattern[SAVER_PATH_MAX], folder[SAVER_PATH_MAX], xbe[SAVER_PATH_MAX];
    WIN32_FIND_DATA fd;
    HANDLE          h;

    SvJoin(pattern, sizeof(pattern), root, "*");
    h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (s_poolCount >= SAVER_MAX_POOL) break;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        SvJoin(folder, sizeof(folder), root, fd.cFileName);
        SvJoin(xbe, sizeof(xbe), folder, "default.xbe");
        if (GetFileAttributes(xbe) != 0xFFFFFFFF) {
            SvCopy(s_pool[s_poolCount], SAVER_PATH_MAX, xbe);
            s_poolCount++;
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

static void ScanConfig(const LauncherConfig* cfg) {
    int i;
    if (!cfg || !cfg->roots) return;
    for (i = 0; i < cfg->rootCount && s_poolCount < SAVER_MAX_POOL; i++)
        ScanRootInto(cfg->roots[i]);
}

static void BuildPool(void) {
    s_poolCount = 0;
    ScanConfig(App_Config());
    ScanConfig(Games_Config());
    ScanConfig(Homebrew_Config());
    ScanConfig(Emulators_Config());
    s_scanned = 1;
}

/* ---- art cycling ------------------------------------------------------- */

static void FreeArt(void) {
    if (s_haveArt && s_art.tex) Texture_Release(&s_art);
    s_art.tex = NULL;
    s_haveArt = 0;
}

static void PickNext(void) {
    int idx;
    FreeArt();
    if (s_poolCount <= 0) { s_cur = -1; return; }
    /* random, avoid immediately repeating the same title when we can */
    idx = (int)(SvRand() % (unsigned)s_poolCount);
    if (s_poolCount > 1 && idx == s_cur) idx = (idx + 1) % s_poolCount;
    s_cur = idx;
    if (Launcher_LoadArtFor(s_pool[idx], &s_art, &s_artFlat))
        s_haveArt = 1;
    s_phase = SP_FADE_IN;
    s_phaseStart = GetTickCount();
}

/* ---- public ------------------------------------------------------------ */

void Saver_Enter(void) {
    s_enterMs = GetTickCount();
    s_rng ^= (s_enterMs | 1u);
    if (!s_scanned) BuildPool();
    s_cur = -1;
    PickNext();
}

void Saver_Update(void) {
    DWORD now = GetTickCount();
    DWORD el = now - s_phaseStart;
    if (s_phase == SP_FADE_IN) { if (el >= SAVER_FADE_MS) { s_phase = SP_HOLD;    s_phaseStart = now; } }
    else if (s_phase == SP_HOLD) { if (el >= SAVER_HOLD_MS) { s_phase = SP_FADE_OUT; s_phaseStart = now; } }
    else /* FADE_OUT */ { if (el >= SAVER_FADE_MS) { PickNext(); } }
}

void Saver_Render(void) {
    DWORD now = GetTickCount();
    DWORD t = now - s_enterMs;
    DWORD el = now - s_phaseStart;
    int   fade = 255;
    float dxV, dyV;
    int   r = 0, g = 0, b = 0;

    if (!s_haveArt || !s_art.tex) return;

    /* fade envelope */
    if (s_phase == SP_FADE_IN)       fade = (el >= SAVER_FADE_MS) ? 255 : (int)(el * 255u / SAVER_FADE_MS);
    else if (s_phase == SP_FADE_OUT) fade = (el >= SAVER_FADE_MS) ? 0 : 255 - (int)(el * 255u / SAVER_FADE_MS);
    else                             fade = 255;

    /* Drift across the WHOLE screen. The pedestal's authored base (viewport at
       80,95 / platform at 70,270) sits in the launcher's left-hand slot, so we
       add a constant offset to recentre its travel on screen middle, then
       wander widely around that with a slow Lissajous (+/-150 px H, +/-60 px V).
       Periods 23s / 31s don't share a small multiple, so the path looks varied
       and never quite retraces. Bounds keep the 200x220 viewport on-screen. */
    {
        float cx = 140.0f;   /* recentre X: viewport-centre 180 -> ~320 */
        float cy = 20.0f;    /* nudge down a touch */
        dxV = cx + 150.0f * (float)sin((double)t * (6.2831853 / 23000.0));
        dyV = cy + 60.0f * (float)sin((double)t * (6.2831853 / 31000.0));
    }

    /* rainbow hue advances ~one full cycle per ~12s */
    SvHueRgb((unsigned)((t / 8u) % 1536u), &r, &g, &b);

    /* the platform base is a themed 2D sprite (same one the launcher draws at
       70,270). Shift it by the same drift as the 3D pedestal and fade it with
       the title so the whole showpiece moves and dissolves together. */
    {
        const Texture* ped = Theme_Asset("platform_round");
        if (ped)
            UI_DrawSprite(ped, 70.0f + dxV, 270.0f + dyV, 220.0f, 116.0f,
                UI_ARGB(fade, 255, 255, 255), 0);
    }

    Pedestal_DrawSaver(&s_art, s_artFlat, now, dxV, dyV, fade, r, g, b);
}

void Saver_Exit(void) {
    FreeArt();
    s_cur = -1;
}