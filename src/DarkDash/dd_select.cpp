/*---------------------------------------------------------------------------
    dd_select.cpp -- shared animated selection highlight (see dd_select.h).

    A damped spring eases the highlight Y toward the target. A row change
    kicks a restrained scale-pop (overshoot).
    Frame-rate independent: integrates against real elapsed ms.

    Build: float math is fine (dd_ftol supplies __ftol2_sse); file-scope statics.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_select.h"
#include "dd_iso.h"
#include "dd_ui.h"
#include "dd_data.h"

/* --- tuning (snappy spring, restrained visual response) ------------------ */
#define SEL_STIFFNESS   220.0f   /* spring constant (higher = snappier)       */
#define SEL_DAMPING      22.0f   /* damping (lower vs stiffness = more bounce) */
#define SEL_POP_MS      150.0f   /* scale-pop duration on row change          */
#define SEL_POP_AMT       0.025f /* subtle 2.5% rise before settling         */

static int   s_ctx = -1;
static float s_y = 0.0f;   /* eased Y               */
static float s_vy = 0.0f;   /* Y velocity            */
static float s_target = 0.0f;
static DWORD s_lastMs = 0;
static DWORD s_popStart = 0;     /* tick the last row change kicked the pop */
static int   s_haveY = 0;

/* A6 modal focus has independent state: popup selection cannot disturb
   the animated highlight belonging to the underlying menu. */
#if DD_A6_INTERACTION
static int s_modalCtx = -1, s_modalIndex = -1;
static DWORD s_modalChanged = 0, s_modalSeen = 0;
#define SEL_FOCUS_MS 210u
#endif

void Select_Reset(void) {
    s_haveY = 0;          /* next Begin snaps to target */
    s_vy = 0.0f;
    s_popStart = 0;
}

void Select_Begin(int ctxId, float targetY) {
    DWORD now = GetTickCount();
    float dt;

    /* selection FX disabled -> snap instantly, no spring/pop */
    if (!Data_FxOn(DD_FX_SELECT)) {
        s_ctx = ctxId; s_y = targetY; s_target = targetY;
        s_vy = 0.0f; s_haveY = 1; s_popStart = 0; s_lastMs = now;
        return;
    }

    /* context change (different screen): snap, don't slide across layouts */
    if (ctxId != s_ctx) { s_ctx = ctxId; s_haveY = 0; }

    /* first use / after reset: snap to target */
    if (!s_haveY) {
        s_y = targetY; s_vy = 0.0f; s_target = targetY;
        s_haveY = 1; s_lastMs = now; s_popStart = 0;
        return;
    }

    /* row change -> kick the restrained pop */
    if (targetY != s_target) {
        s_target = targetY;
        s_popStart = now;
    }

    /* elapsed time, clamped (avoid huge steps after a stall) */
    dt = (float)(now - s_lastMs) / 1000.0f;
    s_lastMs = now;
    if (dt > 0.05f) dt = 0.05f;
    if (dt <= 0.0f) return;

    /* semi-implicit Euler spring toward target */
    {
        float a = (s_target - s_y) * SEL_STIFFNESS - s_vy * SEL_DAMPING;
        s_vy += a * dt;
        s_y += s_vy * dt;
    }
}

float Select_Y(void) { return s_y; }

/* 0..1 progress of the pop window (0 = none/done) */
static float PopProgress(void) {
    DWORD now = GetTickCount();
    float e;
    if (s_popStart == 0) return 0.0f;
    e = (float)(now - s_popStart);
    if (e >= SEL_POP_MS) return 0.0f;
    return e / SEL_POP_MS;        /* 0..1 */
}

/* The same restrained transparent highlight is used on both isometric and
   flat menus. Draw only solid-colour geometry: the supplied selection mask
   contains opaque white/green decoration and looks jagged/overexposed when
   stretched into a menu row. Never use additive blending for the fill.

   Most callers historically pass alpha 70..110; cap intensity at 100 to keep
   a consistent appearance while still respecting dialog/swing fade-outs. */
static void DrawBoxCommon(float x, float y, float w, float h,
    DWORD baseColor, int flat) {
    int a = (int)((baseColor >> 24) & 0xFF);
    int r = (int)((baseColor >> 16) & 0xFF);
    int g = (int)((baseColor >> 8) & 0xFF);
    int b = (int)(baseColor & 0xFF);
    int fa, edge, rail;
    if (w <= 0.0f || h <= 0.0f || a <= 0) return;
    if (a > 100) a = 100;
    fa = (54 * a + 50) / 100;     /* 21% fill at full intensity */
    edge = (98 * a + 50) / 100;  /* 38% thin outline */
    rail = (130 * a + 50) / 100; /* left accent, still translucent */

    if (flat) {
        UI_FillRect(x, y, w, h, UI_ARGB(fa, r, g, b));
        UI_FillRect(x, y, w, 1.0f, UI_ARGB(edge, r, g, b));
        UI_FillRect(x, y + h - 1.0f, w, 1.0f, UI_ARGB(edge, r, g, b));
        UI_FillRect(x + w - 1.0f, y, 1.0f, h, UI_ARGB(edge, r, g, b));
        UI_FillRect(x, y, 2.0f, h, UI_ARGB(rail, r, g, b));
    }
    else {
        Iso_FillRect(x, y, w, h, UI_ARGB(fa, r, g, b), 0);
        Iso_FillRect(x, y, w, 1.0f, UI_ARGB(edge, r, g, b), 0);
        Iso_FillRect(x, y + h - 1.0f, w, 1.0f, UI_ARGB(edge, r, g, b), 0);
        Iso_FillRect(x + w - 1.0f, y, 1.0f, h, UI_ARGB(edge, r, g, b), 0);
        Iso_FillRect(x, y, 2.0f, h, UI_ARGB(rail, r, g, b), 0);
    }
}

/* Short, low-alpha moving reflection on a freshly changed selection.
   Uses the selected row's own theme colour and inherits modal/swing alpha.
   Transparent fill and text remain unchanged (never additive blending). */
#if DD_A6_INTERACTION
static void DrawFocusFlash(float x, float y, float w, float h, DWORD baseColor,
    int flat, DWORD changedAt) {
    DWORD elapsed;
    int a, r, g, b, flash;
    float p, fx;
    if (!changedAt || !Data_FxOn(DD_FX_SELECT) || w < 36.0f || h < 8.0f) return;
    elapsed = GetTickCount() - changedAt;
    if (elapsed >= SEL_FOCUS_MS) return;
    a = (int)((baseColor >> 24) & 255);
    if (a > 100) a = 100;
    flash = (16 * a * (int)(SEL_FOCUS_MS - elapsed)) / (100 * (int)SEL_FOCUS_MS);
    if (flash < 1) return;
    r = (int)((baseColor >> 16) & 255); g = (int)((baseColor >> 8) & 255);
    b = (int)(baseColor & 255);
    p = (float)elapsed / (float)SEL_FOCUS_MS;
    fx = x + 5.0f + (w - 20.0f) * p;
    if (flat)
        UI_FillGradient(fx, y + 2.0f, 8.0f, h - 4.0f,
            UI_ARGB(0, r, g, b), UI_ARGB(flash, r, g, b),
            UI_ARGB(0, r, g, b), UI_ARGB(flash, r, g, b), 0);
    else
        Iso_FillRect(fx, y + 2.0f, 2.0f, h - 4.0f,
            UI_ARGB(flash, r, g, b), 0);
}
#endif

static void DrawGlowCommon(float x, float y, float w, float h,
    DWORD baseColor, int flat) {
    float p, ease, scale, cx, cy, sw, sh;
    (void)y; /* eased Y from Select_Begin overrides the passed Y */

    /* Shared spring/pop movement for flat and isometric menus. */
    p = PopProgress();
    ease = (p > 0.0f) ? (4.0f * p * (1.0f - p)) : 0.0f;
    scale = 1.0f + SEL_POP_AMT * ease;
    cx = x + w * 0.5f;
    cy = s_y + h * 0.5f;
    sw = w * scale;
    sh = h * scale;
    DrawBoxCommon(cx - sw * 0.5f, cy - sh * 0.5f,
        sw, sh, baseColor, flat);
#if DD_A6_INTERACTION
    DrawFocusFlash(cx - sw * 0.5f, cy - sh * 0.5f, sw, sh, baseColor, flat, s_popStart);
#endif
}

void Select_DrawGlow(float x, float y, float w, float h, DWORD baseColor) {
    DrawGlowCommon(x, y, w, h, baseColor, 0);
}

void Select_DrawGlowFlat(float x, float y, float w, float h, DWORD baseColor) {
    DrawGlowCommon(x, y, w, h, baseColor, 1);
}

void Select_DrawBoxFlat(float x, float y, float w, float h, DWORD baseColor) {
    /* Modal rows don't animate; they shouldn't perturb the shared spring. */
    DrawBoxCommon(x, y, w, h, baseColor, 1);
}

void Select_BeginModal(int ctxId, int selectedIndex) {
#if DD_A6_INTERACTION
    DWORD now = GetTickCount();
    if (!Data_FxOn(DD_FX_SELECT)) { s_modalChanged = 0; return; }
    if (ctxId != s_modalCtx || selectedIndex != s_modalIndex ||
        now - s_modalSeen > 450u) {
        s_modalCtx = ctxId; s_modalIndex = selectedIndex; s_modalChanged = now;
    }
    s_modalSeen = now;
#else
    (void)ctxId; (void)selectedIndex;
#endif
}

void Select_DrawModalBoxFlat(float x, float y, float w, float h, DWORD baseColor) {
    DrawBoxCommon(x, y, w, h, baseColor, 1);
#if DD_A6_INTERACTION
    if (GetTickCount() - s_modalSeen < 80u)
        DrawFocusFlash(x, y, w, h, baseColor, 1, s_modalChanged);
#endif
}
