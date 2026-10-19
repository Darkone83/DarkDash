/*--------------------------------------------------------------------------
    dd_hints.cpp -- Xbox controller glyphs and measured footer tokens.

    No texture/font-atlas glyphs are added: A/B/X/Y are painted as geometry,
    so even custom .ddf fonts retain correct button appearance. This module
    never changes the underlying input mappings.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <math.h>
#include "dd_ui.h"
#include "dd_theme.h"
#include "font.h"
#include "dd_hints.h"

#define DD_HINT_TOKEN_MAX 48
#define DD_HINT_SMALL FONT_SIZE_SMALL
#define DD_HINT_ICON_H 15.0f

enum {
    HINT_TEXT = 0, HINT_A, HINT_B, HINT_X, HINT_Y,
    HINT_WHITE, HINT_BLACK, HINT_START, HINT_BACK,
    HINT_L3, HINT_R3, HINT_LT, HINT_RT, HINT_LTRT, HINT_LR
};

typedef struct {
    char text[DD_HINT_TOKEN_MAX];
    int gap;                /* whitespace immediately preceding this token */
    int kind;
} HintToken;

static char HintUpper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}
static int HintEq(const char* word, const char* token) {
    int i = 0;
    while (word[i] && token[i]) {
        if (HintUpper(word[i]) != token[i]) return 0;
        i++;
    }
    return !word[i] && !token[i];
}
static int HintKind(const char* w, int previous) {
    if (HintEq(w, "A")) return HINT_A;
    if (HintEq(w, "B")) return HINT_B;
    if (HintEq(w, "X")) return HINT_X;
    if (HintEq(w, "Y")) return HINT_Y;
    if (HintEq(w, "WHITE")) return HINT_WHITE;
    if (HintEq(w, "BLACK")) return HINT_BLACK;
    if (HintEq(w, "START")) return HINT_START;
    /* In "B BACK", BACK is the action label, not another key glyph. */
    if (HintEq(w, "BACK") && previous != HINT_B) return HINT_BACK;
    if (HintEq(w, "L3")) return HINT_L3;
    if (HintEq(w, "R3")) return HINT_R3;
    if (HintEq(w, "LT")) return HINT_LT;
    if (HintEq(w, "RT")) return HINT_RT;
    if (HintEq(w, "LT/RT")) return HINT_LTRT;
    if (HintEq(w, "L/R")) return HINT_LR;
    return HINT_TEXT;
}

/* Split on whitespace only. No heap allocation, no reliance on CRT locale. */
static const char* HintNext(const char* p, HintToken* out, int previous) {
    int n = 0, gap = 0;
    out->text[0] = 0; out->gap = 0; out->kind = HINT_TEXT;
    if (!p) return NULL;
    while (*p == ' ' || *p == '\t') { ++gap; ++p; }
    if (!*p) return NULL;
    while (*p && *p != ' ' && *p != '\t') {
        if (n < DD_HINT_TOKEN_MAX - 1) out->text[n++] = *p;
        ++p;
    }
    out->text[n] = 0;
    out->gap = gap;
    out->kind = HintKind(out->text, previous);
    return p;
}

static float HintGap(int gap) {
    return gap >= 2 ? 11.0f : (gap > 0 ? 4.0f : 0.0f);
}
static float HintWidth(const HintToken* t) {
    int w;
    if (t->kind == HINT_TEXT) {
        w = Font_MeasureText(t->text, DD_HINT_SMALL);
        return w > 0 ? (float)w : 0.0f;
    }
    if (t->kind == HINT_A || t->kind == HINT_B ||
        t->kind == HINT_X || t->kind == HINT_Y) return 16.0f;
    {
        float minimum = 23.0f;
        float label = (float)Font_MeasureText(t->text, DD_HINT_SMALL) + 6.0f;
        if (t->kind == HINT_WHITE || t->kind == HINT_BLACK) minimum = 33.0f;
        if (t->kind == HINT_LTRT) minimum = 39.0f;
        if (t->kind == HINT_START) minimum = 34.0f;
        if (t->kind == HINT_BACK) minimum = 31.0f;
        return label > minimum ? label : minimum;
    }
}

float DD_Hints_Measure(const char* hint) {
    HintToken t;
    const char* p = hint;
    float width = 0.0f;
    int prev = HINT_TEXT, first = 1;
    while ((p = HintNext(p, &t, prev)) != NULL) {
        if (!first) width += HintGap(t.gap);
        width += HintWidth(&t);
        prev = t.kind; first = 0;
    }
    return width;
}

static void HintCircle(float cx, float cy, float radius, DWORD color) {
    /* 12-segment disc using the same calibrated UI triangle path as chrome. */
    static const float px[13] = {
        1.000f, 0.866f, 0.500f, 0.000f, -0.500f, -0.866f,
        -1.000f, -0.866f, -0.500f, 0.000f, 0.500f, 0.866f, 1.000f
    };
    static const float py[13] = {
        0.000f, 0.500f, 0.866f, 1.000f, 0.866f, 0.500f,
        0.000f, -0.500f, -0.866f, -1.000f, -0.866f, -0.500f, 0.000f
    };
    int i;
    for (i = 0; i < 12; i++)
        UI_FillTri(cx, cy, cx + px[i] * radius, cy + py[i] * radius,
            cx + px[i + 1] * radius, cy + py[i + 1] * radius, color);
}

/* Fixed vector letter strokes. These do not depend on the installed .ddf
   font, even if the font lacks controller glyphs or uses extreme metrics. */
static void HintStroke(float x1, float y1, float x2, float y2, float width, DWORD c) {
    float dx = x2 - x1, dy = y2 - y1;
    float length = (float)sqrt((double)(dx * dx + dy * dy));
    float nx, ny;
    if (length < 0.01f) return;
    nx = -dy * width * 0.5f / length;
    ny = dx * width * 0.5f / length;
    UI_FillTri(x1 + nx, y1 + ny, x2 + nx, y2 + ny, x1 - nx, y1 - ny, c);
    UI_FillTri(x1 - nx, y1 - ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, c);
}
/* In a modal, icon bodies and lettering must fade with the modal's text.
   The atlas-free glyphs previously stayed fully opaque until the last frame. */
static DWORD HintFade(DWORD colour, DWORD parent) {
    unsigned a = (colour >> 24) & 255u;
    unsigned pa = (parent >> 24) & 255u;
    return (colour & 0x00FFFFFFUL) | (((a * pa + 127u) / 255u) << 24);
}
static void HintLetter(float x, float y, int kind, DWORD parent) {
    DWORD ink = HintFade(kind == HINT_Y ? 0xFF13171B : 0xFFFDFDFD, parent);
    /* A letter is approximately 8x10 pixels centered inside its disc. */
    x += 4.0f; y += 2.6f;
    switch (kind) {
    case HINT_A:
        HintStroke(x, y + 10.0f, x + 4.0f, y, 1.9f, ink);
        HintStroke(x + 4.0f, y, x + 8.0f, y + 10.0f, 1.9f, ink);
        HintStroke(x + 2.1f, y + 6.6f, x + 5.9f, y + 6.6f, 1.6f, ink);
        break;
    case HINT_B:
        HintStroke(x + 0.7f, y, x + 0.7f, y + 10.0f, 1.8f, ink);
        HintStroke(x + 0.7f, y + 0.7f, x + 6.6f, y + 0.7f, 1.6f, ink);
        HintStroke(x + 0.7f, y + 5.0f, x + 6.5f, y + 5.0f, 1.6f, ink);
        HintStroke(x + 0.7f, y + 9.3f, x + 6.6f, y + 9.3f, 1.6f, ink);
        HintStroke(x + 6.6f, y + 0.7f, x + 6.6f, y + 4.4f, 1.7f, ink);
        HintStroke(x + 6.6f, y + 5.4f, x + 6.6f, y + 9.3f, 1.7f, ink);
        break;
    case HINT_X:
        HintStroke(x + 0.5f, y + 0.5f, x + 7.5f, y + 9.5f, 2.1f, ink);
        HintStroke(x + 7.5f, y + 0.5f, x + 0.5f, y + 9.5f, 2.1f, ink);
        break;
    case HINT_Y:
        HintStroke(x + 0.5f, y + 0.5f, x + 4.0f, y + 4.8f, 2.0f, ink);
        HintStroke(x + 7.5f, y + 0.5f, x + 4.0f, y + 4.8f, 2.0f, ink);
        HintStroke(x + 4.0f, y + 4.8f, x + 4.0f, y + 9.6f, 2.0f, ink);
        break;
    }
}

static void HintDrawToken(IDirect3DDevice8* d, float x, float y,
    const HintToken* t, DWORD color) {
    const int kind = t->kind;
    const float w = HintWidth(t);
    if (kind == HINT_TEXT) {
        Font_DrawText(d, x, y + (DD_HINT_ICON_H - (float)Font_GlyphHeight(DD_HINT_SMALL)) * 0.5f,
            t->text, DD_HINT_SMALL, color, 0);
        return;
    }
    if (kind >= HINT_A && kind <= HINT_Y) {
        DWORD bg = kind == HINT_A ? 0xFF3DA94B :
            kind == HINT_B ? 0xFFD54848 :
            kind == HINT_X ? 0xFF3B84D4 : 0xFFE5BD38;
        HintCircle(x + 8.0f, y + DD_HINT_ICON_H * 0.5f, 7.5f, HintFade(bg, color));
        HintLetter(x, y, kind, color);
        return;
    }
    {
        DWORD bg = kind == HINT_WHITE ? 0xFFF0F0F0 : 0xFF30343D;
        DWORD fg = kind == HINT_WHITE ? 0xFF181818 : 0xFFE5E8EF;
        DWORD edge = kind == HINT_BLACK ? 0xFFB0B5C0 : 0xFF747987;
        const char* label = t->text;
        float th = (float)Font_GlyphHeight(DD_HINT_SMALL);
        float tw = (float)Font_MeasureText(label, DD_HINT_SMALL);
        UI_FillRect(x, y, w, DD_HINT_ICON_H, HintFade(edge, color));
        UI_FillRect(x + 1.0f, y + 1.0f, w - 2.0f, DD_HINT_ICON_H - 2.0f, HintFade(bg, color));
        Font_DrawText(d, x + (w - tw) * 0.5f, y + (DD_HINT_ICON_H - th) * 0.5f,
            label, DD_HINT_SMALL, HintFade(fg, color), 0);
    }
}

/* If an exceptionally wide custom font exhausts the safe area, preserve
   the primary action and the FINAL B action instead of clipping B BACK/CANCEL.
   Only simplify ordinary <button> <verb> prompts; leave arbitrary help text
   untouched. Buffers are local, and the existing .ddf format is unchanged. */
static const char* HintFit(const char* hint, float maxWidth, char* buf, int cap) {
    HintToken t, next;
    const char* p = hint;
    const char* q;
    char firstKey[DD_HINT_TOKEN_MAX], firstVerb[DD_HINT_TOKEN_MAX];
    char lastVerb[DD_HINT_TOKEN_MAX];
    int gotFirst = 0;
    int gotLast = 0;
    int i, n;
    if (!hint || DD_Hints_Measure(hint) <= maxWidth) return hint;
    firstKey[0] = firstVerb[0] = lastVerb[0] = 0;
    while (p && (q = HintNext(p, &t, HINT_TEXT)) != NULL) {
        /* The next token is parsed with the preceding button kind, so the
           word BACK in 'B BACK' is a verb, not a second controller icon. */
        const char* after = HintNext(q, &next, t.kind);
        if (after && next.kind == HINT_TEXT) {
            if (!gotFirst && t.kind >= HINT_A && t.kind <= HINT_R3) {
                n = 0;
                for (i = 0; t.text[i] && n < DD_HINT_TOKEN_MAX - 1; ++i) firstKey[n++] = t.text[i];
                firstKey[n] = 0;
                n = 0;
                for (i = 0; next.text[i] && n < DD_HINT_TOKEN_MAX - 1; ++i) firstVerb[n++] = next.text[i];
                firstVerb[n] = 0;
                gotFirst = 1;
            }
            if (t.kind == HINT_B && !*after &&
                (HintEq(next.text, "BACK") || HintEq(next.text, "CANCEL") ||
                    HintEq(next.text, "CLOSE") || HintEq(next.text, "EXIT") ||
                    HintEq(next.text, "NO") || HintEq(next.text, "UP"))) {
                n = 0;
                for (i = 0; next.text[i] && n < DD_HINT_TOKEN_MAX - 1; ++i) lastVerb[n++] = next.text[i];
                lastVerb[n] = 0;
                gotLast = 1;
            }
        }
        p = q;
    }
    if (!gotLast) return hint;
    n = 0;
    if (gotFirst && !(HintEq(firstKey, "B") && HintEq(firstVerb, lastVerb))) {
        const char* parts[4] = { firstKey, " ", firstVerb, "  " };
        for (i = 0; i < 4; ++i) {
            const char* s = parts[i];
            while (*s && n < cap - 1) buf[n++] = *s++;
        }
    }
    if (n < cap - 1) buf[n++] = 'B';
    if (n < cap - 1) buf[n++] = ' ';
    for (i = 0; lastVerb[i] && n < cap - 1; ++i) buf[n++] = lastVerb[i];
    buf[n] = 0;
    if (DD_Hints_Measure(buf) <= maxWidth) return buf;
    n = 0;
    buf[n++] = 'B'; buf[n++] = ' ';
    for (i = 0; lastVerb[i] && n < cap - 1; ++i) buf[n++] = lastVerb[i];
    buf[n] = 0;
    return DD_Hints_Measure(buf) <= maxWidth ? buf : hint;
}

void DD_Hints_Draw(IDirect3DDevice8* device, float x, float top,
    float maxWidth, float height, const char* hint, DWORD textColor) {
    HintToken t;
    const char* p;
    char fitted[2 * DD_HINT_TOKEN_MAX + 16];
    float pos = 0.0f;
    float y = top + (height - DD_HINT_ICON_H) * 0.5f;
    int prev = HINT_TEXT, first = 1;
    if (!device || !hint || maxWidth <= 0.0f) return;
    p = HintFit(hint, maxWidth, fitted, sizeof(fitted));
    while ((p = HintNext(p, &t, prev)) != NULL) {
        float gap = first ? 0.0f : HintGap(t.gap);
        float next = pos + gap + HintWidth(&t);
        if (next > maxWidth) break;  /* never paint beyond the footer safe area */
        pos += gap;
        HintDrawToken(device, x + pos, y, &t, textColor);
        pos = next; prev = t.kind; first = 0;
    }
}

void DD_Hints_Footer(IDirect3DDevice8* device, float barTop, float barHeight,
    float hintX, float maxWidth, const char* hint, DWORD textColor) {
    const Texture* art;
    if (!device) return;
    art = Theme_RoleAsset("ui.footer.frame");
#if DD_VISUAL_IMPACT
    /* Universal command dock shadow and theme-colored rim, below its text. */
    UI_FillGradient(14.0f, barTop - 9.0f, 612.0f, 9.0f,
        UI_ARGB(0, 0, 0, 0), UI_ARGB(0, 0, 0, 0),
        UI_ARGB(108, 0, 0, 0), UI_ARGB(108, 0, 0, 0), 0);
#endif
    if (art) UI_DrawNineSlice(art, 8.0f, barTop, 624.0f, barHeight,
        26.0f, 12.0f, 26.0f, 12.0f, 0xFFFFFFFF, 0);
#if DD_VISUAL_IMPACT
    /* Draw after authored footer art so even fully opaque themed bars get a
       readable illuminated edge. Never touch the controller glyph colors. */
    {
        DWORD rim = Theme_Color("glow", 0xFFAEFF3C);
        int r = (rim >> 16) & 255, g = (rim >> 8) & 255, b = rim & 255;
        UI_FillGradient(20.0f, barTop + 3.0f, 600.0f, 2.0f,
            UI_ARGB(62, r, g, b), UI_ARGB(15, r, g, b),
            UI_ARGB(62, r, g, b), UI_ARGB(15, r, g, b), 0);
    }
#endif
    DD_Hints_Draw(device, hintX, barTop, maxWidth, barHeight, hint, textColor);
}
