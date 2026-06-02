/*---------------------------------------------------------------------------
    dd_select.cpp -- shared animated selection highlight (see dd_select.h).

    A critically-ish-damped spring eases the highlight Y toward the target. A
    row change kicks a short scale-pop (overshoot) and a brief chromatic split.
    Frame-rate independent: integrates against real elapsed ms.

    Build: float math is fine (dd_ftol supplies __ftol2_sse); file-scope statics.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_select.h"
#include "dd_iso.h"
#include "dd_ui.h"
#include "dd_data.h"

/* --- tuning (snappy + edgy: fast spring, sharp little overshoot) --------- */
#define SEL_STIFFNESS   220.0f   /* spring constant (higher = snappier)       */
#define SEL_DAMPING      22.0f   /* damping (lower vs stiffness = more bounce) */
#define SEL_POP_MS      150.0f   /* scale-pop duration on row change          */
#define SEL_POP_AMT       0.06f  /* peak extra scale (1.0 -> 1.06)            */
#define SEL_CHROMA_MS   130.0f   /* chromatic-split duration on row change    */
#define SEL_CHROMA_PX     3.0f   /* peak chromatic offset (virtual px)        */

static int   s_ctx = -1;
static float s_y = 0.0f;   /* eased Y               */
static float s_vy = 0.0f;   /* Y velocity            */
static float s_target = 0.0f;
static DWORD s_lastMs = 0;
static DWORD s_popStart = 0;     /* tick the last row change kicked the pop */
static int   s_haveY = 0;

void Select_Reset(void) {
    s_haveY = 0;          /* next Begin snaps to target */
    s_vy = 0.0f;
    s_popStart = 0;
}

void Select_Begin(int ctxId, float targetY) {
    DWORD now = GetTickCount();
    float dt;

    /* selection FX disabled -> snap instantly, no spring/pop/chromatic */
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

    /* row change -> kick the pop + chromatic tick */
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

void Select_DrawGlow(float x, float y, float w, float h, DWORD baseColor) {
    float p, ease, scale, cx, cy, sw, sh;
    int   a = (int)((baseColor >> 24) & 0xFF);
    int   r = (int)((baseColor >> 16) & 0xFF);
    int   g = (int)((baseColor >> 8) & 0xFF);
    int   b = (int)(baseColor & 0xFF);
    (void)y;   /* eased Y from Select_Begin overrides the passed y */

    /* pop scale: quick rise then settle (sine-ish via 4*p*(1-p) hump) */
    p = PopProgress();
    ease = (p > 0.0f) ? (4.0f * p * (1.0f - p)) : 0.0f;   /* 0->1->0 hump */
    scale = 1.0f + SEL_POP_AMT * ease;

    /* scale about the rect center */
    cx = x + w * 0.5f;
    cy = s_y + h * 0.5f;
    sw = w * scale;
    sh = h * scale;

    /* chromatic split during the same window: draw R and B ghosts offset
       opposite ways (additive), then the main glow on top */
    {
        DWORD chN = GetTickCount() - s_popStart;
        float cp = (s_popStart && chN < (DWORD)SEL_CHROMA_MS)
            ? (1.0f - (float)chN / SEL_CHROMA_MS) : 0.0f;
        if (cp > 0.0f) {
            float off = SEL_CHROMA_PX * cp;
            int   ga = (int)(a * 0.55f);
            Iso_FillRect(cx - sw * 0.5f - off, cy - sh * 0.5f,
                sw, sh, UI_ARGB(ga, r, 0, 0), 1);
            Iso_FillRect(cx - sw * 0.5f + off, cy - sh * 0.5f,
                sw, sh, UI_ARGB(ga, 0, 0, b), 1);
        }
    }

    /* main highlight */
    Iso_FillRect(cx - sw * 0.5f, cy - sh * 0.5f, sw, sh, UI_ARGB(a, r, g, b), 1);
}