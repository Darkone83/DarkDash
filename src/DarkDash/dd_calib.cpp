/*---------------------------------------------------------------------------
    dd_calib.cpp -- screen calibration overlay (see dd_calib.h).

    Two-stick corner drag: the LEFT stick drags the top-left safe-area corner,
    the RIGHT stick drags the bottom-right corner. Push a stick toward where you
    want that corner to sit -- out toward the screen edge to widen, back toward
    center to pull in. Deflection sets the speed (gentle near center for fine
    nudges, quicker at full push), so bringing a margin in and back out again is
    one fluid motion. A saves, B cancels.

    During the run loop we zero the UI calibration so UI_Sx/Sy map the full
    640x480 virtual canvas to the whole screen; the corners are drawn at the
    chosen inset against full-screen reference lines. On save we push the insets
    into DD_Settings and apply them live via UI_SetCalibration.

    Build: MSVC2003/C89 style; file-scope statics; no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_calib.h"
#include "dd_gfx.h"
#include "dd_ui.h"
#include "font.h"
#include "input.h"
#include "dd_audio.h"
#include "dd_data.h"
#include "dd_theme.h"
#include "dd_texture.h"

#define CALIB_RATE_MAX 4.0f   /* px/frame at full stick deflection */

static float s_l, s_r, s_t, s_b;   /* working insets (virtual px, fractional) */

static float ClampF(float v, float lo, float hi) {
    if (v < lo) return lo; if (v > hi) return hi; return v;
}

/* int -> decimal string (no CRT) */
static void ItoA(int v, char* out, int cap) {
    char tmp[12]; int n = 0, i = 0; if (cap <= 0) return;
    if (v < 0) v = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0 && i < cap - 1) out[i++] = tmp[--n];
    out[i] = 0;
}

/* stick axis (-32767..32767, already deadzoned) -> px/frame. Quadratic, so it
   creeps near center for fine alignment and moves quickly toward full push. */
static float StickRate(int v) {
    float n = (float)v / 32767.0f;        /* -1..1 */
    float m = (n < 0.0f) ? -n : n;        /* magnitude */
    float r = m * m * CALIB_RATE_MAX;
    return (n < 0.0f) ? -r : r;
}

void Calib_Apply(void) {
    DD_Settings* s = Data_Get();
    UI_SetCalibration((float)s->calibL, (float)s->calibR,
        (float)s->calibT, (float)s->calibB);
}

int Calib_NeedsRun(void) {
    DD_Settings* s = Data_Get();
    return s->calibrated ? 0 : 1;
}

void Calib_Run(void) {
    DD_Settings* s = Data_Get();
    IDirect3DDevice8* d;
    WORD prev;
    int running = 1, save = 0;

    /* seed working values from saved (or 0) */
    s_l = ClampF((float)s->calibL, 0.0f, (float)CALIB_MAX);
    s_r = ClampF((float)s->calibR, 0.0f, (float)CALIB_MAX);
    s_t = ClampF((float)s->calibT, 0.0f, (float)CALIB_MAX);
    s_b = ClampF((float)s->calibB, 0.0f, (float)CALIB_MAX);

    /* seed prev with whatever is held right now (e.g. the A press that opened
       this screen) so it isn't read as an immediate Save-and-exit on frame 1. */
    PumpInput();
    prev = GetButtons();

    /* draw with NO inset so the corners map against the true screen */
    UI_SetCalibration(0.0f, 0.0f, 0.0f, 0.0f);
    d = Gfx_Device();

    while (running) {
        WORD btn, pressed;
        int  lx, ly, rx, ry;
        DWORD t; int pv; DWORD col, coldim;
        DWORD accent, glow, text;
        int   gr, gg, gb;

        PumpInput();
        btn = GetButtons();
        pressed = (WORD)(btn & ~prev);
        prev = btn;

        /* Left stick drags the top-left corner, right stick the bottom-right.
           Push toward where you want the corner to go; the signs below map a
           stick deflection to the matching edge inset (X right grows the left
           inset / shrinks the right; Y up shrinks the top inset / grows the
           bottom). */
        GetSticks(lx, ly, rx, ry);
        s_l = ClampF(s_l + StickRate(lx), 0.0f, (float)CALIB_MAX);
        s_t = ClampF(s_t - StickRate(ly), 0.0f, (float)CALIB_MAX);
        s_r = ClampF(s_r - StickRate(rx), 0.0f, (float)CALIB_MAX);
        s_b = ClampF(s_b + StickRate(ry), 0.0f, (float)CALIB_MAX);

        if (pressed & BTN_A) { save = 1; running = 0; Audio_PlaySfx(SFX_SELECT); }
        if (pressed & BTN_B) { save = 0; running = 0; Audio_PlaySfx(SFX_BACK); }

        /* ---- render (own present loop; black screen) ---- */
        Gfx_BeginFrame(0xFF000000);

        /* themed colors (follow the active theme) */
        accent = Theme_Color("accent", 0xFF7FE000);
        glow = Theme_Color("glow", 0xFFAEFF3C);
        text = Theme_Color("text", 0xFFD8F8C0);
        gr = (int)((glow >> 16) & 0xFF); gg = (int)((glow >> 8) & 0xFF); gb = (int)(glow & 0xFF);

        /* pulsing alpha for the two stick-dragged corner handles */
        t = GetTickCount() % 1600;
        {
            float bphase = (float)t / 1600.0f;
            pv = (bphase < 0.5f) ? (int)(bphase * 2.0f * 130.0f)
                : (int)((2.0f - bphase * 2.0f) * 130.0f);
        }
        col = UI_ARGB((DWORD)(125 + pv), gr, gg, gb);   /* TL & BR: active handles  */
        coldim = UI_ARGB(90, gr, gg, gb);                  /* TR & BL + frame: passive */

        /* the safe-area rectangle: a dim frame whose edges meet the corner
           handles, so the whole shape drags together with the sticks. Corner
           triangles sit on top -- the two stick handles (top-left, bottom-right)
           pulse bright; the other two stay dim. */
        {
            float L = s_l, R = 640.0f - s_r;
            float T = s_t, B = 480.0f - s_b;
            float w = R - L, h = B - T;
            float a = 34.0f;   /* corner leg length */

            /* frame edges, aligned to the inset corners */
            UI_FillRect(L, T, w, 1.0f, coldim);          /* top    */
            UI_FillRect(L, B, w, 1.0f, coldim);          /* bottom */
            UI_FillRect(L, T, 1.0f, h, coldim);          /* left   */
            UI_FillRect(R, T, 1.0f, h, coldim);          /* right  */

            /* corner handles (right-angle at the inset corner, hypotenuse to center) */
            UI_FillTri(L, T, L + a, T, L, T + a, col);      /* top-left  (left stick)     */
            UI_FillTri(R, B, R - a, B, R, B - a, col);      /* bottom-right (right stick) */
            UI_FillTri(R, T, R - a, T, R, T + a, coldim);   /* top-right  (passive)       */
            UI_FillTri(L, B, L + a, B, L, B - a, coldim);   /* bottom-left (passive)      */
        }

        /* title + instructions, themed, centered */
        Font_DrawTextCentered(d, 0.0f, 150.0f, 640.0f, "SCREEN CALIBRATION",
            FONT_SIZE_MEDIUM, accent);
        Font_DrawTextCentered(d, 0.0f, 198.0f, 640.0f,
            "Left stick: drag the top-left corner",
            FONT_SIZE_SMALL, text);
        Font_DrawTextCentered(d, 0.0f, 220.0f, 640.0f,
            "Right stick: drag the bottom-right corner",
            FONT_SIZE_SMALL, text);

        /* current values, themed */
        {
            char vals[64], n[8];
            int li = (int)(s_l + 0.5f), ri = (int)(s_r + 0.5f);
            int ti = (int)(s_t + 0.5f), bi = (int)(s_b + 0.5f);
            vals[0] = 0;
            lstrcatA(vals, "L "); ItoA(li, n, sizeof(n)); lstrcatA(vals, n);
            lstrcatA(vals, "   R "); ItoA(ri, n, sizeof(n)); lstrcatA(vals, n);
            lstrcatA(vals, "   T "); ItoA(ti, n, sizeof(n)); lstrcatA(vals, n);
            lstrcatA(vals, "   B "); ItoA(bi, n, sizeof(n)); lstrcatA(vals, n);
            Font_DrawTextCentered(d, 0.0f, 248.0f, 640.0f, vals,
                FONT_SIZE_SMALL, glow);
        }

        /* footer hint bar, like the other screens */
        {
            const Texture* foot = Theme_Asset("bar_footer");
            if (foot) UI_DrawSprite(foot, 8.0f, 442.0f, 624.0f, 32.0f, 0xFFFFFFFF, 0);
            Font_DrawTextCentered(d, 8.0f, 449.0f, 624.0f,
                "STICKS  DRAG CORNERS      A  SAVE      B  CANCEL",
                FONT_SIZE_SMALL, text);
        }

        Gfx_EndFrame();
        Audio_Update();
    }

    if (save) {
        s->calibL = (int)(s_l + 0.5f); s->calibR = (int)(s_r + 0.5f);
        s->calibT = (int)(s_t + 0.5f); s->calibB = (int)(s_b + 0.5f);
        s->calibrated = 1;
        Data_Save();
    }

    /* apply whatever is now in settings (saved values, or prior on cancel) */
    Calib_Apply();
}