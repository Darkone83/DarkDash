/*---------------------------------------------------------------------------
    dd_osk.cpp -- controller on-screen keyboard (see dd_osk.h).

    Adapted from SceneChat's screen_kb to DarkDash's primitives: UI_FillRect
    for panels (+ a small outline helper, no rounded corners), Theme_Color for
    palette, Font_DrawText/Centered/Right for labels. Authored in the 640x480
    virtual space; the ui scaler maps it to the active resolution. Adds a
    numeric 10-key layout alongside the full QWERTY one.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <string.h>
#include "dd_osk.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "font.h"
#include "input.h"

typedef struct { char sets[3]; int wide; } Key;

#define MAX_KEYS_PER_ROW 13

/* special-key sentinels (stored in sets[0]) */
#define SK_SPACE  '\x01'
#define SK_BKSP   '\x02'
#define SK_DONE   '\x03'
#define SK_CANCEL '\x04'

/* ---- text (QWERTY) layout -------------------------------------------------*/
static const Key k_t0[MAX_KEYS_PER_ROW] = {
    {{'1','1','!'},1},{{'2','2','@'},1},{{'3','3','#'},1},{{'4','4','$'},1},
    {{'5','5','%'},1},{{'6','6','^'},1},{{'7','7','&'},1},{{'8','8','*'},1},
    {{'9','9','('},1},{{'0','0',')'},1},{{'-','-','_'},1},{{'=','=','+'},1},{{0,0,0},0}
};
static const Key k_t1[MAX_KEYS_PER_ROW] = {
    {{'q','Q','q'},1},{{'w','W','w'},1},{{'e','E','e'},1},{{'r','R','r'},1},
    {{'t','T','t'},1},{{'y','Y','y'},1},{{'u','U','u'},1},{{'i','I','i'},1},
    {{'o','O','o'},1},{{'p','P','p'},1},{{'[','[','{'},1},{{']',']','}'},1},{{0,0,0},0}
};
static const Key k_t2[MAX_KEYS_PER_ROW] = {
    {{'a','A','a'},1},{{'s','S','s'},1},{{'d','D','d'},1},{{'f','F','f'},1},
    {{'g','G','g'},1},{{'h','H','h'},1},{{'j','J','j'},1},{{'k','K','k'},1},
    {{'l','L','l'},1},{{';',';',':'},1},{{'\'','\'','"'},1},{{0,0,0},0}
};
static const Key k_t3[MAX_KEYS_PER_ROW] = {
    {{'z','Z','z'},1},{{'x','X','x'},1},{{'c','C','c'},1},{{'v','V','v'},1},
    {{'b','B','b'},1},{{'n','N','n'},1},{{'m','M','m'},1},{{',',',','<'},1},
    {{'.','.','>'},1},{{'/','/','?'},1},{{0,0,0},0}
};
static const Key k_t4[MAX_KEYS_PER_ROW] = {
    {{SK_SPACE,SK_SPACE,SK_SPACE},4},{{SK_BKSP,SK_BKSP,SK_BKSP},2},
    {{SK_DONE,SK_DONE,SK_DONE},2},{{SK_CANCEL,SK_CANCEL,SK_CANCEL},2},{{0,0,0},0}
};
static const Key* const k_text[5] = { k_t0, k_t1, k_t2, k_t3, k_t4 };
static const int        k_textCount[5] = { 12, 12, 11, 10, 4 };

/* ---- numeric 10-key layout ------------------------------------------------*/
static const Key k_n0[MAX_KEYS_PER_ROW] = { {{'1','1','1'},1},{{'2','2','2'},1},{{'3','3','3'},1},{{0,0,0},0} };
static const Key k_n1[MAX_KEYS_PER_ROW] = { {{'4','4','4'},1},{{'5','5','5'},1},{{'6','6','6'},1},{{0,0,0},0} };
static const Key k_n2[MAX_KEYS_PER_ROW] = { {{'7','7','7'},1},{{'8','8','8'},1},{{'9','9','9'},1},{{0,0,0},0} };
static const Key k_n3[MAX_KEYS_PER_ROW] = { {{'.','.','.'},1},{{'0','0','0'},1},{{SK_BKSP,SK_BKSP,SK_BKSP},1},{{0,0,0},0} };
static const Key k_n4[MAX_KEYS_PER_ROW] = { {{SK_DONE,SK_DONE,SK_DONE},2},{{SK_CANCEL,SK_CANCEL,SK_CANCEL},1},{{0,0,0},0} };
static const Key* const k_num[5] = { k_n0, k_n1, k_n2, k_n3, k_n4 };
static const int        k_numCount[5] = { 3, 3, 3, 3, 2 };

#define ROWS 5

/* ---- layout geometry (virtual 640x480) ------------------------------------*/
#define KB_TEXT_X   40.0f
#define KB_TEXT_Y   150.0f
#define KB_TEXT_H   40.0f
#define KB_KEY_H    34.0f
#define KB_KEY_GAP  4.0f
#define KB_ROW_GAP  4.0f

/* panel differs per layout so the numeric pad is compact + centered */
static float s_panelX, s_panelY, s_panelW;

/* ---- state ----------------------------------------------------------------*/
static int  s_open = 0;
static int  s_mode = OSK_TEXT;
static int  s_keyset = 0;
static int  s_row = 0;
static int  s_col = 0;
static int  s_maxLen = OSK_MAX_LEN;
static char s_text[OSK_MAX_LEN + 1];
static int  s_len = 0;
static int  s_blink = 0;
static WORD s_lastBtn = 0;
static int  s_repeat = 0;
#define REPEAT_INITIAL 18
#define REPEAT_RATE    5

static const Key* const* Rows(void) { return (s_mode == OSK_NUMERIC) ? k_num : k_text; }
static const int* Counts(void) { return (s_mode == OSK_NUMERIC) ? k_numCount : k_textCount; }
static int RowCount(int r) { return Counts()[r]; }

static void ClampCol(void) {
    int n = RowCount(s_row);
    if (s_col >= n) s_col = n - 1;
    if (s_col < 0)  s_col = 0;
}

static void LayoutForMode(void) {
    if (s_mode == OSK_NUMERIC) {
        s_panelW = 240.0f;
        s_panelX = (UI_VIRT_W - s_panelW) * 0.5f;
        s_panelY = 210.0f;
    }
    else {
        s_panelX = KB_TEXT_X;
        s_panelW = UI_VIRT_W - KB_TEXT_X * 2.0f;
        s_panelY = 210.0f;
    }
}

/* ---- public ---------------------------------------------------------------*/

void Osk_Open(int mode, const char* initial, int maxLen) {
    int len;
    s_open = 1; s_mode = mode; s_keyset = 0; s_row = 0; s_col = 0;
    s_maxLen = (maxLen > OSK_MAX_LEN) ? OSK_MAX_LEN : maxLen;
    s_lastBtn = 0; s_repeat = 0; s_blink = 0;
    LayoutForMode();
    if (initial) {
        len = (int)lstrlenA(initial);
        if (len > s_maxLen) len = s_maxLen;
        memcpy(s_text, initial, len); s_text[len] = 0; s_len = len;
    }
    else { s_text[0] = 0; s_len = 0; }
}

void Osk_Close(void) { s_open = 0; }
int  Osk_IsOpen(void) { return s_open; }

void Osk_GetText(char* buf, int buflen) {
    int n = (s_len < buflen - 1) ? s_len : buflen - 1;
    memcpy(buf, s_text, n); buf[n] = 0;
}

static void TypeKey(int row, int col) {
    char ch = Rows()[row][col].sets[s_keyset];
    switch (ch) {
    case SK_SPACE: if (s_len < s_maxLen) { s_text[s_len++] = ' '; s_text[s_len] = 0; } break;
    case SK_BKSP:  if (s_len > 0) { s_len--; s_text[s_len] = 0; } break;
    case SK_DONE:  break;
    case SK_CANCEL:break;
    default:
        if (ch >= 0x20 && s_len < s_maxLen) {
            s_text[s_len++] = ch; s_text[s_len] = 0;
            if (s_keyset == 1) s_keyset = 0;   /* one-shot shift */
        }
        break;
    }
}

int Osk_Update(WORD pressed) {
    WORD held;
    if (!s_open) return 0;
    s_blink++;

    held = GetButtons();
    if (held == s_lastBtn && held != 0) {
        s_repeat++;
        if (s_repeat < REPEAT_INITIAL) pressed = 0;
        else if ((s_repeat - REPEAT_INITIAL) % REPEAT_RATE != 0) pressed = 0;
    }
    else { s_repeat = 0; s_lastBtn = held; }

    if (pressed & BTN_DPAD_UP) { s_row = (s_row + ROWS - 1) % ROWS; ClampCol(); }
    if (pressed & BTN_DPAD_DOWN) { s_row = (s_row + 1) % ROWS;        ClampCol(); }
    if (pressed & BTN_DPAD_LEFT) { s_col = (s_col + RowCount(s_row) - 1) % RowCount(s_row); }
    if (pressed & BTN_DPAD_RIGHT) { s_col = (s_col + 1) % RowCount(s_row); }

    if (pressed & (BTN_A | BTN_LTRIG)) {
        char action = Rows()[s_row][s_col].sets[s_keyset];
        if (action == SK_DONE) { s_open = 0; return  1; }
        if (action == SK_CANCEL) { s_open = 0; return -1; }
        TypeKey(s_row, s_col);
    }
    if (pressed & BTN_B) { if (s_len > 0) { s_len--; s_text[s_len] = 0; } }
    if ((pressed & BTN_X) && s_mode == OSK_TEXT) s_keyset = (s_keyset + 1) % 3;
    if ((pressed & BTN_Y) && s_mode == OSK_TEXT) { if (s_len < s_maxLen) { s_text[s_len++] = ' '; s_text[s_len] = 0; } }
    if (pressed & BTN_START) { s_open = 0; return  1; }
    if (pressed & BTN_BACK) { s_open = 0; return -1; }
    return 0;
}

/* ---- draw -----------------------------------------------------------------*/

static void Outline(float x, float y, float w, float h, DWORD c) {
    UI_FillRect(x, y, w, 1.0f, c);
    UI_FillRect(x, y + h - 1.0f, w, 1.0f, c);
    UI_FillRect(x, y, 1.0f, h, c);
    UI_FillRect(x + w - 1.0f, y, 1.0f, h, c);
}

static void KeyRect(int row, int col, float* ox, float* ow) {
    const Key* keys = Rows()[row];
    int n = RowCount(row), i;
    float units = 0.0f, unit, cx = s_panelX;
    for (i = 0; i < n; i++) units += (float)keys[i].wide;
    unit = (s_panelW - KB_KEY_GAP * (float)(n - 1)) / units;
    for (i = 0; i < col; i++) cx += (float)keys[i].wide * unit + KB_KEY_GAP;
    *ox = cx; *ow = (float)keys[col].wide * unit;
}

void Osk_Draw(IDirect3DDevice8* d) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    int   row, col, n, gh;
    float kx, ky, kw, tx, tw;
    char  label[4];

    if (!s_open) return;

    /* dim the whole screen */
    UI_FillRect(0.0f, 0.0f, UI_VIRT_W, UI_VIRT_H, 0xCC000000);

    /* text field */
    tx = (s_mode == OSK_NUMERIC) ? s_panelX : KB_TEXT_X;
    tw = (s_mode == OSK_NUMERIC) ? s_panelW : (UI_VIRT_W - KB_TEXT_X * 2.0f);
    UI_FillRect(tx, KB_TEXT_Y, tw, KB_TEXT_H, 0xFF101418);
    Outline(tx, KB_TEXT_Y, tw, KB_TEXT_H, dim);
    {
        char disp[OSK_MAX_LEN + 2];
        gh = Font_GlyphHeight(FONT_SIZE_MEDIUM);
        lstrcpyA(disp, s_text);
        if ((s_blink / 20) % 2 == 0) lstrcatA(disp, "_");
        Font_DrawText(d, tx + 10.0f, KB_TEXT_Y + (KB_TEXT_H - (float)gh) * 0.5f,
            disp, FONT_SIZE_MEDIUM, text, (int)(tw - 20.0f));
    }

    /* keys */
    gh = Font_GlyphHeight(FONT_SIZE_MEDIUM);
    for (row = 0; row < ROWS; row++) {
        n = RowCount(row);
        ky = s_panelY + (float)row * (KB_KEY_H + KB_ROW_GAP);
        for (col = 0; col < n; col++) {
            const Key* k = &Rows()[row][col];
            char ch = k->sets[s_keyset];
            int sel = (row == s_row && col == s_col);
            DWORD bg, fg, bd;

            KeyRect(row, col, &kx, &kw);

            if (sel) { bg = accent; fg = 0xFF101418; bd = accent; }
            else {
                bd = dim;
                if (ch == SK_DONE) { bg = 0xFF14361A; fg = accent; }
                else if (ch == SK_CANCEL) { bg = 0xFF361414; fg = 0xFFE05050; }
                else { bg = 0xFF181C1F; fg = text; }
            }
            UI_FillRect(kx, ky, kw, KB_KEY_H, bg);
            Outline(kx, ky, kw, KB_KEY_H, bd);

            switch (ch) {
            case SK_SPACE:  lstrcpyA(label, "SP"); break;
            case SK_BKSP:   lstrcpyA(label, "<X"); break;
            case SK_DONE:   lstrcpyA(label, "OK"); break;
            case SK_CANCEL: lstrcpyA(label, "X");  break;
            default:        label[0] = ch; label[1] = 0; break;
            }
            Font_DrawTextCentered(d, kx, ky + (KB_KEY_H - (float)gh) * 0.5f, kw,
                label, FONT_SIZE_MEDIUM, fg);
        }
    }

    Font_DrawTextCentered(d, 0.0f, s_panelY + ROWS * (KB_KEY_H + KB_ROW_GAP) + 8.0f,
        UI_VIRT_W,
        (s_mode == OSK_NUMERIC)
        ? "A Type   B Del   Start OK   Back Cancel"
        : "A Type  B Del  X Case  Y Space  Start OK  Back Cancel",
        FONT_SIZE_SMALL, dim);
}