/*---------------------------------------------------------------------------
    dd_backdrop.cpp -- shared ambient green bloom (see dd_backdrop.h).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <math.h>
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "dd_backdrop.h"
#include "dd_data.h"

static Texture s_bloom = { 0, 0, 0, 0, 0 };

int Backdrop_Init(void) {
    return Texture_CreateRadialGlow(128, &s_bloom);
}

/* Every screen shares this backdrop. The home screen also supplies a focused
   row for parallax; other screens drift gently without moving UI geometry.
   All light and motion derive from existing theme settings. */
static float s_homeFocus = 0.5f;
static DWORD s_homeFocusAt = 0;

static float clamp01(float v) {
    if (!(v >= 0.0f)) return 0.0f;
    return v > 1.0f ? 1.0f : v;
}

static void draw_backdrop(int home, float focus) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
#if DD_A4_DEPTH
    DWORD muted = Theme_Color("accent_dim", 0xFF4C8A00);
    const ThemeStageLighting* lighting = Theme_StageLighting();
#endif
    int ar = (int)((accent >> 16) & 0xFF);
    int ag = (int)((accent >> 8) & 0xFF);
    int ab = (int)(accent & 0xFF);
#if DD_A4_DEPTH
    int mr = (int)((muted >> 16) & 0xFF);
    int mg = (int)((muted >> 8) & 0xFF);
    int mb = (int)(muted & 0xFF);
#endif
    DWORD t = GetTickCount();
    int phase = (int)((t >> 3) & 255);
    int tri = (phase < 128) ? phase : (255 - phase);
    int glowA = 24 + (tri >> 2);
    const Texture* bg = Theme_BackgroundImage();
    float wx = 0.0f, wy = 0.0f;

    if (home) {
        DWORD delta = s_homeFocusAt ? (t - s_homeFocusAt) : 16u;
        float smoothing;
        if (delta > 80u) delta = 80u;
        s_homeFocusAt = t;
        smoothing = (float)delta * 0.008f;
        if (smoothing > 0.42f) smoothing = 0.42f;
        s_homeFocus += (clamp01(focus) - s_homeFocus) * smoothing;
    }
    else {
        /* Secondary screens still get the environmental treatment; their
           artwork drifts gently without moving functional controls. */
        s_homeFocusAt = 0;
        s_homeFocus = 0.5f;
    }

    if (bg && bg->tex) {
#if DD_A4_DEPTH
        float parallax = Theme_BackgroundParallax() *
            (float)Data_EffectPercent(DD_EFFECT_PARALLAX) / 100.0f;
#else
        float parallax = 0.0f;
#endif
        if (parallax > 2.0f) parallax = 2.0f;
        if (parallax > 0.0f && Data_FxOn(DD_FX_IDLE)) {
            /* Overscan the sprite so translations never expose clear-color
               borders. This is a background image, not UI text or geometry. */
            float pad = 12.0f * parallax;
            float pan = home ? (s_homeFocus - 0.5f) * 2.0f
                : (float)sin((double)t * 0.00022) * 0.22f;
            UI_DrawSprite(bg, -pad - pan * pad * 0.60f,
                -pad - pan * pad * 0.38f,
                640.0f + 2.0f * pad, 480.0f + 2.0f * pad,
                0xFFFFFFFF, 0);
        }
        else UI_DrawSprite(bg, 0.0f, 0.0f, 640.0f, 480.0f, 0xFFFFFFFF, 0);
    }

#if DD_A4_DEPTH && DD_A5_LIGHTING
    {
        /* Soft directional colour washes behind the entire UI. Uses the
           existing A3 ambient/key settings; no render targets or new assets. */
        DWORD ambient = lighting->ambient;
        DWORD key = lighting->key;
        int lr = (ambient >> 16) & 255, lg = (ambient >> 8) & 255, lb = ambient & 255;
        int kr = (key >> 16) & 255, kg = (key >> 8) & 255, kb = key & 255;
        int strength = lighting->ambientLevel;
        int alpha = 10 + strength / 25;
        if (alpha > 17) alpha = 17;
        alpha = Data_EffectScale(DD_EFFECT_ATMOSPHERE, alpha);
        UI_FillGradient(0.0f, 0.0f, 640.0f, 480.0f,
            UI_ARGB(alpha, lr, lg, lb), UI_ARGB(alpha / 3, kr, kg, kb),
            UI_ARGB(alpha / 2, lr, lg, lb), UI_ARGB(0, lr, lg, lb), 0);
    }
#endif

    /* Every functional screen shares depth, light and atmosphere.
       The original broad bloom is retained, supplemented by restrained pools
       behind left/right interfaces. Home alone gets a brighter stage-floor
       halo; all screen text and controls remain stationary. */
    if (Data_FxOn(DD_FX_IDLE)) {
        float a = (float)(t % 23000u) / 23000.0f * 6.2831853f;
        float b = (float)(t % 17000u) / 17000.0f * 6.2831853f;
        wx = 28.0f * (float)cos((double)a);
        wy = 20.0f * (float)sin((double)b);
    }

    if (s_bloom.tex) {
        UI_DrawSprite(&s_bloom, -40.0f + wx, -40.0f + wy,
            720.0f, 560.0f, UI_ARGB(Data_EffectScale(DD_EFFECT_ATMOSPHERE,
                34 + (glowA >> 2)), ar, ag, ab), 1);
#if DD_A4_DEPTH
        {
            float drift = home && Data_FxOn(DD_FX_IDLE)
                ? (s_homeFocus - 0.5f) * 12.0f : 0.0f;
            int ambientA = 14 + lighting->ambientLevel / 14;
            int stageA = 18 + lighting->beamLevel / 15;
            if (ambientA > 26) ambientA = 26;
            if (stageA > 32) stageA = 32;
            ambientA = Data_EffectScale(DD_EFFECT_ATMOSPHERE, ambientA);
            stageA = Data_EffectScale(DD_EFFECT_LIGHTING, stageA);
            /* Light behind all main/submenu panels, not over live labels. */
#if DD_A5_LIGHTING
            {
                DWORD amb = lighting->ambient;
                int lr = (int)((amb >> 16) & 255), lg = (int)((amb >> 8) & 255), lb = (int)(amb & 255);
                /* Blend low-energy ambient into the existing stage accent.
                   Still tied to each theme's palette, never a fixed green. */
                UI_DrawSprite(&s_bloom, 286.0f + drift, -10.0f,
                    410.0f, 490.0f,
                    UI_ARGB(ambientA, (mr * 3 + lr) / 4, (mg * 3 + lg) / 4, (mb * 3 + lb) / 4), 1);
            }
#else
            UI_DrawSprite(&s_bloom, 286.0f + drift, -10.0f,
                410.0f, 490.0f, UI_ARGB(ambientA, mr, mg, mb), 1);
#endif
            /* All screens receive a muted left counter-light; home has a
               slightly stronger grounded platform light around the orb. */
            UI_DrawSprite(&s_bloom, 12.0f - drift * 0.35f, 248.0f,
                342.0f, 200.0f, UI_ARGB(home ? stageA : stageA / 2, ar, ag, ab), 1);
        }
#endif
    }
}

void Backdrop_Draw(void) { draw_backdrop(0, 0.5f); }
void Backdrop_DrawHome(float focus) { draw_backdrop(1, focus); }

void Backdrop_Shutdown(void) {
    if (s_bloom.tex) Texture_Release(&s_bloom);
}