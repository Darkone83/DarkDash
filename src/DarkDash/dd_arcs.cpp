/*---------------------------------------------------------------------------
    dd_arcs.cpp -- see dd_arcs.h.

    A tiny pool of short-lived "Jacob's ladder" bolts. Each bolt spans a portion
    of one frame edge (endpoints on the border, alpha-tapered so they vanish into
    the frame), bowing outward by a small parabola plus a flickering jagged
    displacement -- both clamped so the bolt never strays more than ARC_MAX_OUT
    virtual units (~px) outside the quad. Built as a vertex-coloured triangle-
    strip ribbon and drawn additively in the iso plane via Iso_DrawStrip, so it
    tilts with the frame. No CRT math: parabola arch (no sin), edge-perpendicular
    ribbon (no sqrt), LCG noise. C89 style.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_arcs.h"
#include "dd_iso.h"
#include "dd_theme.h"

/* ---- tunables ---------------------------------------------------------- */
#define ARC_MAX        3           /* concurrent bolts                       */
#define ARC_SEG        40          /* path segments (high-poly = smooth)     */
#define ARC_PTS        (ARC_SEG + 1)
#define ARC_VERTS      (ARC_PTS * 2)
#define ARC_MAX_OUT    18.0f       /* max excursion OUTSIDE the quad (~px)   */
#define ARC_TUCK        5.0f       /* pull anchors INSIDE the border (~px) so
                                      they tuck behind the frame instead of
                                      floating in the inset gap on the sides   */
#define ARC_HALF_W     1.8f        /* ribbon half-width (radial)             */
#define ARC_JAG        5.0f        /* jagged displacement amplitude          */
#define ARC_JAG_MS     45          /* re-roll the crackle every N ms         */
#define ARC_ALPHA      210         /* peak core alpha (0..255)               */
#define SPAWN_MIN_MS   1500
#define SPAWN_MAX_MS   4000
#define LIFE_MIN_MS    240
#define LIFE_MAX_MS    430

typedef struct {
    int           active;
    int           edge;            /* 0 top, 1 bottom, 2 left, 3 right       */
    float         f0, f1;          /* span as fractions [0..1] along the edge */
    float         bow;             /* outward arch peak (<= ARC_MAX_OUT)      */
    unsigned long born, life;
    unsigned      seed;
} Arc;

static Arc           s_arc[ARC_MAX];
static int           s_seeded = 0;
static unsigned long s_nextSpawn = 0;
static unsigned      s_rng = 0x1234567u;

/* ---- tiny rng (LCG) ---------------------------------------------------- */
static unsigned Rnd(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng; }
static float    Rnd01(void) { return (float)(Rnd() >> 8) / (float)(1u << 24); }
static float    RndRange(float a, float b) { return a + (b - a) * Rnd01(); }

/* deterministic per-(seed,segment,phase) noise in [-1,1] -- stable within a
   frame, re-rolls as the phase advances so the bolt flickers. */
static float Jag(unsigned seed, int i, unsigned phase) {
    unsigned h = seed ^ ((unsigned)i * 2654435761u) ^ (phase * 40503u);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (float)(h >> 8) / (float)(1u << 24) * 2.0f - 1.0f;
}

static void SpawnArc(unsigned long now) {
    int slot = -1, i;
    Arc* a;
    float span, start;
    for (i = 0; i < ARC_MAX; i++) if (!s_arc[i].active) { slot = i; break; }
    if (slot < 0) return;
    a = &s_arc[slot];
    span = RndRange(0.18f, 0.42f);             /* fraction of the edge       */
    start = RndRange(0.06f, 0.94f - span);
    a->active = 1;
    a->edge = (int)(Rnd() % 4u);
    a->f0 = start;
    a->f1 = start + span;
    a->bow = RndRange(ARC_MAX_OUT * 0.65f, ARC_MAX_OUT);
    a->born = now;
    a->life = (unsigned long)RndRange((float)LIFE_MIN_MS, (float)LIFE_MAX_MS);
    a->seed = Rnd();
}

void Arcs_Tick(unsigned long now) {
    int i;
    if (!s_seeded) {
        s_rng ^= ((unsigned)now | 1u);
        s_seeded = 1;
        s_nextSpawn = now + 500;
    }
    for (i = 0; i < ARC_MAX; i++)
        if (s_arc[i].active && (now - s_arc[i].born) >= s_arc[i].life)
            s_arc[i].active = 0;

    if ((long)(now - s_nextSpawn) >= 0) {
        int n = 1 + ((Rnd() % 5u == 0u) ? 1 : 0);   /* occasional double pop */
        int k;
        for (k = 0; k < n; k++) SpawnArc(now);
        s_nextSpawn = now + (unsigned long)RndRange((float)SPAWN_MIN_MS, (float)SPAWN_MAX_MS);
    }
}

void Arcs_Draw(float fx, float fy, float fw, float fh) {
    unsigned long now = GetTickCount();
    DWORD  glowC = Theme_Color("glow", 0xFFAEFF3C);
    int    gr = (int)((glowC >> 16) & 0xFF);
    int    gg = (int)((glowC >> 8) & 0xFF);
    int    gb = (int)(glowC & 0xFF);
    int    ai;

    for (ai = 0; ai < ARC_MAX; ai++) {
        Arc* a;
        IsoStripPt strip[ARC_VERTS];
        unsigned long el;
        unsigned phase;
        float tt, fade;
        int i;

        if (!s_arc[ai].active) continue;
        a = &s_arc[ai];

        el = now - a->born;
        if (el >= a->life) continue;
        tt = (float)el / (float)a->life;                 /* 0..1 over life     */
        /* life fade: quick ramp in, longer ramp out */
        if (tt < 0.20f) fade = tt / 0.20f;
        else if (tt > 0.55f) fade = (1.0f - tt) / 0.45f;
        else                 fade = 1.0f;
        if (fade < 0.0f) fade = 0.0f; else if (fade > 1.0f) fade = 1.0f;
        phase = (unsigned)(el / ARC_JAG_MS);

        for (i = 0; i < ARC_PTS; i++) {
            float t = (float)i / (float)ARC_SEG;       /* 0..1 along bolt   */
            float frac = a->f0 + (a->f1 - a->f0) * t;
            float arch = 4.0f * t * (1.0f - t);            /* parabola, peak@.5 */
            float out = a->bow * arch + Jag(a->seed, i, phase) * ARC_JAG;
            float tap, cxp, cyp;
            int   alpha;
            DWORD col;

            /* tuck the anchors inside the border: strongest at the ends, zero at
               the peak (scales with 1-arch), so the outward reach is untouched. */
            out -= ARC_TUCK * (1.0f - arch);

            if (out < -ARC_TUCK) out = -ARC_TUCK;          /* allow a little inside */
            else if (out > ARC_MAX_OUT) out = ARC_MAX_OUT;

            /* end taper: fade the bolt into the frame at both anchors */
            if (t < 0.12f) tap = t / 0.12f;
            else if (t > 0.88f) tap = (1.0f - t) / 0.12f;
            else                tap = 1.0f;

            alpha = (int)((float)ARC_ALPHA * fade * tap);
            if (alpha < 0) alpha = 0; else if (alpha > 255) alpha = 255;
            col = ((DWORD)alpha << 24) | ((DWORD)gr << 16) | ((DWORD)gg << 8) | (DWORD)gb;

            /* centre point on the edge, pushed OUTWARD by 'out'; ribbon widens
               radially (along the outward axis) by +/- ARC_HALF_W */
            switch (a->edge) {
            case 0: /* top    */ cxp = fx + frac * fw; cyp = fy - out;
                strip[i * 2 + 0].vx = cxp; strip[i * 2 + 0].vy = cyp - ARC_HALF_W;
                strip[i * 2 + 1].vx = cxp; strip[i * 2 + 1].vy = cyp + ARC_HALF_W;
                break;
            case 1: /* bottom */ cxp = fx + frac * fw; cyp = fy + fh + out;
                strip[i * 2 + 0].vx = cxp; strip[i * 2 + 0].vy = cyp - ARC_HALF_W;
                strip[i * 2 + 1].vx = cxp; strip[i * 2 + 1].vy = cyp + ARC_HALF_W;
                break;
            case 2: /* left   */ cyp = fy + frac * fh; cxp = fx - out;
                strip[i * 2 + 0].vx = cxp - ARC_HALF_W; strip[i * 2 + 0].vy = cyp;
                strip[i * 2 + 1].vx = cxp + ARC_HALF_W; strip[i * 2 + 1].vy = cyp;
                break;
            default:/* right  */ cyp = fy + frac * fh; cxp = fx + fw + out;
                strip[i * 2 + 0].vx = cxp - ARC_HALF_W; strip[i * 2 + 0].vy = cyp;
                strip[i * 2 + 1].vx = cxp + ARC_HALF_W; strip[i * 2 + 1].vy = cyp;
                break;
            }
            strip[i * 2 + 0].colour = col;
            strip[i * 2 + 1].colour = col;
        }

        Iso_DrawStrip(strip, ARC_VERTS, 1);   /* additive glow */
    }
}