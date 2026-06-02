/*---------------------------------------------------------------------------
    dd_backdrop.cpp -- shared ambient green bloom (see dd_backdrop.h).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "dd_backdrop.h"

static Texture s_bloom = { 0, 0, 0, 0, 0 };

int Backdrop_Init(void) {
    return Texture_CreateRadialGlow(128, &s_bloom);
}

void Backdrop_Draw(void) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    int   ar = (int)((accent >> 16) & 0xFF);
    int   ag = (int)((accent >> 8) & 0xFF);
    int   ab = (int)(accent & 0xFF);
    DWORD t = GetTickCount();
    int   phase = (int)((t >> 3) & 255);
    int   tri = (phase < 128) ? phase : (255 - phase);
    int   glowA = 24 + (tri >> 2);          /* same breathe as the main menu */
    const Texture* bg = Theme_BackgroundImage();

    /* optional painted background fills the virtual frame behind everything;
       absent -> the solid palette.bg clear color shows through and the bloom
       provides the ambient look. */
    if (bg && bg->tex)
        UI_DrawSprite(bg, 0.0f, 0.0f, 640.0f, 480.0f, 0xFFFFFFFF, 0);

    if (s_bloom.tex)
        UI_DrawSprite(&s_bloom, -40.0f, -40.0f, 720.0f, 560.0f,
            UI_ARGB(34 + (glowA >> 2), ar, ag, ab), 1);
}

void Backdrop_Shutdown(void) {
    if (s_bloom.tex) Texture_Release(&s_bloom);
}