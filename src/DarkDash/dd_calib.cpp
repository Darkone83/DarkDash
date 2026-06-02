/*---------------------------------------------------------------------------
    dd_calib.cpp -- screen calibration overlay (see dd_calib.h).

    During the run loop we zero the UI calibration so UI_Sx/Sy map the full
    640x480 virtual canvas to the whole screen; the brackets are then drawn at
    the chosen inset (calibL/T .. 640-calibR/480-calibB) against full-screen
    reference lines. On save we push the insets into DD_Settings and apply them
    live via UI_SetCalibration.

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

static int s_l, s_r, s_t, s_b;   /* working insets (virtual px) */

static int ClampI(int v, int lo, int hi) {
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
    WORD prev = 0;
    int running = 1, save = 0;

    /* seed working values from saved (or 0) */
    s_l = ClampI(s->calibL, 0, CALIB_MAX);
    s_r = ClampI(s->calibR, 0, CALIB_MAX);
    s_t = ClampI(s->calibT, 0, CALIB_MAX);
    s_b = ClampI(s->calibB, 0, CALIB_MAX);

    /* draw with NO inset so brackets map against the true screen */
    UI_SetCalibration(0.0f, 0.0f, 0.0f, 0.0f);
    d = Gfx_Device();

    while (running) {
        WORD btn, pressed;
        int step;
        DWORD t; int pv; DWORD col;
        float rx, bphase;
        DWORD accent, glow, text;
        int   gr, gg, gb;

        PumpInput();
        btn = GetButtons();
        pressed = (WORD)(btn & ~prev);
        prev = btn;

        /* step size: RT = coarse(4), default fine(1). BLACK reverses (subtract). */
        step = (btn & BTN_RTRIG) ? 4 : 1;
        if (btn & BTN_BLACK) step = -step;

        if (pressed & BTN_DPAD_LEFT) { s_l = ClampI(s_l + step, 0, CALIB_MAX); Audio_PlaySfx(SFX_NAV_UP); }
        if (pressed & BTN_DPAD_RIGHT) { s_r = ClampI(s_r + step, 0, CALIB_MAX); Audio_PlaySfx(SFX_NAV_UP); }
        if (pressed & BTN_DPAD_UP) { s_t = ClampI(s_t + step, 0, CALIB_MAX); Audio_PlaySfx(SFX_NAV_UP); }
        if (pressed & BTN_DPAD_DOWN) { s_b = ClampI(s_b + step, 0, CALIB_MAX); Audio_PlaySfx(SFX_NAV_UP); }

        if (pressed & BTN_A) { save = 1; running = 0; Audio_PlaySfx(SFX_SELECT); }
        if (pressed & BTN_B) { save = 0; running = 0; Audio_PlaySfx(SFX_BACK); }

        /* ---- render (own present loop; black screen) ---- */
        Gfx_BeginFrame(0xFF000000);

        /* themed colors (follow the active theme) */
        accent = Theme_Color("accent", 0xFF7FE000);
        glow = Theme_Color("glow", 0xFFAEFF3C);
        text = Theme_Color("text", 0xFFD8F8C0);
        gr = (int)((glow >> 16) & 0xFF); gg = (int)((glow >> 8) & 0xFF); gb = (int)(glow & 0xFF);

        /* faint reference border at the absolute screen edges */
        UI_FillRect(0.0f, 0.0f, 640.0f, 1.0f, UI_ARGB(120, gr, gg, gb));
        UI_FillRect(0.0f, 479.0f, 640.0f, 1.0f, UI_ARGB(120, gr, gg, gb));
        UI_FillRect(0.0f, 0.0f, 1.0f, 480.0f, UI_ARGB(120, gr, gg, gb));
        UI_FillRect(639.0f, 0.0f, 1.0f, 480.0f, UI_ARGB(120, gr, gg, gb));

        /* pulsing alpha for the corner triangles (glow color) */
        t = GetTickCount() % 1600;
        bphase = (float)t / 1600.0f;
        pv = (bphase < 0.5f) ? (int)(bphase * 2.0f * 130.0f)
            : (int)((2.0f - bphase * 2.0f) * 130.0f);
        col = UI_ARGB((DWORD)(125 + pv), gr, gg, gb);

        /* themed corner triangles at the chosen safe-area insets.
           Each is a right triangle with the RIGHT-ANGLE at the corner (the
           alignment point) and the hypotenuse facing screen center. */
        {
            float L = (float)s_l, R = 640.0f - (float)s_r;
            float T = (float)s_t, B = 480.0f - (float)s_b;
            float a = 34.0f;   /* leg length */
            /* top-left  (corner at L,T) */
            UI_FillTri(L, T, L + a, T, L, T + a, col);
            /* top-right (corner at R,T) */
            UI_FillTri(R, T, R - a, T, R, T + a, col);
            /* bottom-left (corner at L,B) */
            UI_FillTri(L, B, L + a, B, L, B - a, col);
            /* bottom-right (corner at R,B) */
            UI_FillTri(R, B, R - a, B, R, B - a, col);
            rx = R; (void)rx;
        }

        /* title + instructions, themed, centered */
        Font_DrawTextCentered(d, 0.0f, 150.0f, 640.0f, "SCREEN CALIBRATION",
            FONT_SIZE_MEDIUM, accent);
        Font_DrawTextCentered(d, 0.0f, 200.0f, 640.0f,
            "Line up each corner with your screen edges",
            FONT_SIZE_SMALL, text);

        /* current values, themed */
        {
            char vals[64], n[8];
            vals[0] = 0;
            lstrcatA(vals, "L "); ItoA(s_l, n, sizeof(n)); lstrcatA(vals, n);
            lstrcatA(vals, "   R "); ItoA(s_r, n, sizeof(n)); lstrcatA(vals, n);
            lstrcatA(vals, "   T "); ItoA(s_t, n, sizeof(n)); lstrcatA(vals, n);
            lstrcatA(vals, "   B "); ItoA(s_b, n, sizeof(n)); lstrcatA(vals, n);
            Font_DrawTextCentered(d, 0.0f, 234.0f, 640.0f, vals,
                FONT_SIZE_SMALL, glow);
        }

        /* footer hint bar, like the other screens */
        {
            const Texture* foot = Theme_Asset("bar_footer");
            if (foot) UI_DrawSprite(foot, 8.0f, 442.0f, 624.0f, 32.0f, 0xFFFFFFFF, 0);
            Font_DrawTextCentered(d, 8.0f, 449.0f, 624.0f,
                "D-PAD ADJUST   RT COARSE   BLACK REVERSE   A SAVE   B CANCEL",
                FONT_SIZE_SMALL, text);
        }

        Gfx_EndFrame();
        Audio_Update();
    }

    if (save) {
        s->calibL = s_l; s->calibR = s_r; s->calibT = s_t; s->calibB = s_b;
        s->calibrated = 1;
        Data_Save();
    }

    /* apply whatever is now in settings (saved values, or prior on cancel) */
    Calib_Apply();
}