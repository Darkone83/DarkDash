/*---------------------------------------------------------------------------
    dd_edit.cpp -- see dd_edit.h.

    Data layer ported from XbDiag's FileEdit (byte-perfect save), refined to
    DarkDash C89 style and made self-contained: the line-scan that captures
    each line's exact terminator runs at load time here, so there's no separate
    viewer pre-pass. Per-line text entry is the themed dd_osk -- A on a line
    pops the keyboard pre-filled, confirm writes it back -- so the original
    char/caret machinery is gone.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <stdlib.h>
#include "dd_edit.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_osk.h"
#include "font.h"
#include "input.h"
#include "dd_audio.h"

#define FE_MAX_LINES     2048
#define FE_MAX_LINE_LEN  256
#define FE_FILE_CAP      (512 * 1024)   /* refuse files larger than 512 KB */
#define ED_PATH_MAX      260

typedef struct {
    int bufOffset;     /* byte offset of content in s_rawBuf (-1 = inserted)  */
    int contentLen;    /* content bytes (excluding terminator)                */
    int termLen;       /* terminator bytes: 0, 1 (\n), or 2 (\r\n)            */
} FE_LineInfo;

/* ---- data-layer state --------------------------------------------------- */
static char* s_rawBuf = NULL;       /* owned copy of original file bytes */
static int        s_rawSize = 0;
static FE_LineInfo s_info[FE_MAX_LINES];
static char       s_lines[FE_MAX_LINES][FE_MAX_LINE_LEN];
static int        s_edited[FE_MAX_LINES];
static int        s_deleted[FE_MAX_LINES];
static int        s_lineCount = 0;
static int        s_dirty = 0;
static DWORD      s_lastError = 0;

/* ---- editor (presentation) state ---------------------------------------- */
static int   s_open = 0;
static char  s_path[ED_PATH_MAX];
static int   s_cursor = 0;               /* highlighted logical line index    */
static int   s_scroll = 0;               /* first visible line                */
static float s_panX = 0.0f;              /* horizontal pan of selected line (virt px) */
static int   s_editingLine = -1;         /* line whose OSK is currently up     */
static int   s_confirmExit = 0;          /* "save before exit?" dialog active  */
static int   s_msg = 0;                  /* 0 none,1 saved,2 save-failed        */

/* ---- helpers ------------------------------------------------------------ */

static int ELen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void ECopy(char* dst, const char* src, int cap) {
    int i = 0;
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* case-insensitive ASCII compare of a suffix */
static int EndsWithCI(const char* s, const char* suf) {
    int ls = ELen(s), lf = ELen(suf), i;
    if (lf > ls) return 0;
    for (i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

int Edit_IsEditable(const char* path) {
    if (!path || !path[0]) return 0;
    return (EndsWithCI(path, ".ini") || EndsWithCI(path, ".cfg") ||
        EndsWithCI(path, ".txt")) ? 1 : 0;
}

/* ---- data layer: scan + load -------------------------------------------- */

/* Build the line table from s_rawBuf, capturing each line's exact terminator
   so save can reproduce the original bytes. */
static void BuildLineInfo(void) {
    int p = 0;
    s_lineCount = 0;
    while (p < s_rawSize && s_lineCount < FE_MAX_LINES) {
        int start = p, contentLen, termLen = 0, copy, j;
        while (p < s_rawSize && s_rawBuf[p] != '\r' && s_rawBuf[p] != '\n') p++;
        contentLen = p - start;
        if (p < s_rawSize && s_rawBuf[p] == '\r') { termLen++; p++; }
        if (p < s_rawSize && s_rawBuf[p] == '\n') { termLen++; p++; }

        s_info[s_lineCount].bufOffset = start;
        s_info[s_lineCount].contentLen = contentLen;
        s_info[s_lineCount].termLen = termLen;
        s_edited[s_lineCount] = 0;
        s_deleted[s_lineCount] = 0;

        copy = (contentLen < FE_MAX_LINE_LEN - 1) ? contentLen : FE_MAX_LINE_LEN - 1;
        for (j = 0; j < copy; j++) s_lines[s_lineCount][j] = s_rawBuf[start + j];
        s_lines[s_lineCount][copy] = 0;

        s_lineCount++;

        /* a trailing terminator with no following content is end-of-file, not
           an empty final line -- stop so we don't fabricate a phantom line */
        if (p >= s_rawSize) break;
    }
    if (s_lineCount == 0) {            /* empty file -> one empty line          */
        s_info[0].bufOffset = -1; s_info[0].contentLen = 0; s_info[0].termLen = 0;
        s_lines[0][0] = 0; s_edited[0] = 0; s_deleted[0] = 0;
        s_lineCount = 1;
    }
}

static int LiveCount(void) {
    int n = 0, i;
    for (i = 0; i < s_lineCount; i++) if (!s_deleted[i]) n++;
    return n;
}

/* map a logical (visible) index to a physical line index, skipping deleted */
static int PhysFromVisible(int vis) {
    int i, n = 0;
    for (i = 0; i < s_lineCount; i++) {
        if (s_deleted[i]) continue;
        if (n == vis) return i;
        n++;
    }
    return -1;
}

static void ReplaceLine(int phys, const char* text) {
    const char* orig;
    int changed = 0, k = 0;
    if (phys < 0 || phys >= s_lineCount) return;
    orig = s_lines[phys];
    while (orig[k] || text[k]) { if (orig[k] != text[k]) { changed = 1; break; } k++; }
    if (changed) {
        ECopy(s_lines[phys], text, FE_MAX_LINE_LEN);
        s_edited[phys] = 1;
        s_dirty = 1;
    }
}

static int InsertLineAfter(int phys) {
    int i, at;
    if (s_lineCount >= FE_MAX_LINES) return s_lineCount;
    at = phys + 1;
    for (i = s_lineCount; i > at; i--) {
        s_info[i] = s_info[i - 1];
        s_edited[i] = s_edited[i - 1];
        s_deleted[i] = s_deleted[i - 1];
        ECopy(s_lines[i], s_lines[i - 1], FE_MAX_LINE_LEN);
    }
    s_info[at].bufOffset = -1; s_info[at].contentLen = 0; s_info[at].termLen = 0;
    s_lines[at][0] = 0; s_edited[at] = 1; s_deleted[at] = 0;
    s_lineCount++;
    s_dirty = 1;
    return s_lineCount;
}

static void DeleteLine(int phys) {
    if (phys < 0 || phys >= s_lineCount) return;
    if (LiveCount() <= 1) return;           /* keep at least one line          */
    s_deleted[phys] = 1;
    s_dirty = 1;
}

/* default terminator for an inserted line: nearest preceding original line's */
static void WriteDefaultTerm(HANDLE h, int phys) {
    int i; DWORD w = 0;
    for (i = phys; i >= 0; i--) {
        if (s_info[i].bufOffset >= 0 && s_info[i].termLen > 0) {
            WriteFile(h, s_rawBuf + s_info[i].bufOffset + s_info[i].contentLen,
                (DWORD)s_info[i].termLen, &w, NULL);
            return;
        }
    }
    WriteFile(h, "\r\n", 2, &w, NULL);
}

static int SaveFile(void) {
    HANDLE h;
    int i, ok = 1;
    if (!s_path[0] || LiveCount() == 0) return 0;

    h = CreateFileA(s_path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { s_lastError = GetLastError(); return 0; }

    for (i = 0; i < s_lineCount && ok; i++) {
        DWORD w = 0;
        if (s_deleted[i]) continue;

        if (!s_edited[i] && s_info[i].bufOffset >= 0) {
            /* unchanged -> original content + terminator, byte-for-byte */
            int total = s_info[i].contentLen + s_info[i].termLen;
            if (total > 0) {
                if (!WriteFile(h, s_rawBuf + s_info[i].bufOffset, (DWORD)total, &w, NULL)
                    || w != (DWORD)total) {
                    s_lastError = GetLastError(); ok = 0;
                }
            }
        }
        else {
            int len = ELen(s_lines[i]);
            if (len > 0) {
                if (!WriteFile(h, s_lines[i], (DWORD)len, &w, NULL) || w != (DWORD)len) {
                    s_lastError = GetLastError(); ok = 0;
                }
            }
            if (ok) {
                if (s_info[i].bufOffset >= 0 && s_info[i].termLen > 0) {
                    if (!WriteFile(h, s_rawBuf + s_info[i].bufOffset + s_info[i].contentLen,
                        (DWORD)s_info[i].termLen, &w, NULL)) {
                        s_lastError = GetLastError(); ok = 0;
                    }
                }
                else if (i < s_lineCount - 1) {
                    WriteDefaultTerm(h, i - 1);   /* inserted: inherit terminator */
                }
                /* last line with no original terminator -> write none */
            }
        }
    }

    FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) { s_dirty = 0; s_lastError = 0; }
    return ok;
}

/* ---- public: open / close ---------------------------------------------- */

int Edit_Open(const char* path) {
    HANDLE h; DWORD size = 0, got = 0;

    Edit_Close();
    if (!Edit_IsEditable(path)) return 0;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    size = GetFileSize(h, NULL);
    if (size == 0xFFFFFFFF || size > FE_FILE_CAP) { CloseHandle(h); return 0; }

    s_rawBuf = (char*)malloc(size > 0 ? size : 1);
    if (!s_rawBuf) { CloseHandle(h); return 0; }
    if (size > 0) {
        if (!ReadFile(h, s_rawBuf, size, &got, NULL) || got != size) {
            free(s_rawBuf); s_rawBuf = NULL; CloseHandle(h); return 0;
        }
    }
    CloseHandle(h);
    s_rawSize = (int)size;

    BuildLineInfo();

    ECopy(s_path, path, sizeof(s_path));
    s_cursor = 0; s_scroll = 0; s_panX = 0.0f;
    s_editingLine = -1; s_confirmExit = 0; s_msg = 0;
    s_dirty = 0;
    s_open = 1;
    return 1;
}

void Edit_Close(void) {
    if (s_rawBuf) { free(s_rawBuf); s_rawBuf = NULL; }
    s_rawSize = 0; s_lineCount = 0; s_dirty = 0;
    s_open = 0; s_editingLine = -1; s_confirmExit = 0;
    if (Osk_IsOpen()) Osk_Close();
}

int Edit_IsOpen(void) { return s_open; }

/* ---- layout constants (frame_menu_v interior, proven insets) ------------ */
#define ED_FX   40.0f
#define ED_FY   48.0f
#define ED_FW   560.0f
#define ED_FH   384.0f
#define ED_ROW_TOP   82.0f
#define ED_ROW_DY    22.0f
#define ED_FOOTER_H  34.0f
#define ED_LINENO_W  40.0f                /* width of the line-number gutter   */
#define ED_TEXT_W    (ED_FW - 80.0f - ED_LINENO_W)  /* interior text col width (2x40 inset) */
#define ED_PAN_STEP  48.0f                /* virtual px per L/R press          */

static int VisRows(void) {
    float avail = ED_FH - ED_ROW_TOP - ED_FOOTER_H;
    int r = (int)(avail / ED_ROW_DY);
    if (r < 1) r = 1;
    return r;
}

static void ClampScroll(void) {
    int vis = VisRows(), live = LiveCount();
    if (s_cursor < 0) s_cursor = 0;
    if (s_cursor >= live) s_cursor = live - 1;
    if (s_cursor < s_scroll) s_scroll = s_cursor;
    if (s_cursor >= s_scroll + vis) s_scroll = s_cursor - vis + 1;
    if (s_scroll < 0) s_scroll = 0;
}

/* ---- update ------------------------------------------------------------- */

int Edit_Update(WORD pressed) {
    if (!s_open) return 1;

    /* OSK owns input while it's up */
    if (Osk_IsOpen()) {
        int r = Osk_Update(pressed);
        if (r == 1) {                       /* confirmed -> write the line     */
            char buf[FE_MAX_LINE_LEN];
            Osk_GetText(buf, sizeof(buf));
            if (s_editingLine >= 0) ReplaceLine(s_editingLine, buf);
            s_editingLine = -1;
            s_panX = 0.0f;                  /* content changed -> reset pan    */
        }
        else if (r == -1) {
            s_editingLine = -1;             /* cancelled                       */
        }
        return 0;
    }

    /* "save before exit?" confirm */
    if (s_confirmExit) {
        if (pressed & BTN_A) { SaveFile(); Edit_Close(); return 1; }
        if (pressed & BTN_B) { Edit_Close(); return 1; }     /* discard + exit  */
        if (pressed & BTN_BACK) { s_confirmExit = 0; }       /* cancel exit     */
        return 0;
    }

    if (pressed & BTN_DPAD_DOWN) { s_cursor++; s_panX = 0.0f; ClampScroll(); Audio_PlaySfx(SFX_NAV_DOWN); s_msg = 0; }
    if (pressed & BTN_DPAD_UP) { s_cursor--; s_panX = 0.0f; ClampScroll(); Audio_PlaySfx(SFX_NAV_UP);   s_msg = 0; }

    /* D-pad left/right pans the highlighted line horizontally when it's longer
       than the interior, so long config lines are fully readable. All math in
       virtual px (Font_MeasureText is pre-scale), so it's resolution-independent
       -- the draw applies UI scaling. Pan clamps to [0, overflow]. */
    if (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT)) {
        int phys = PhysFromVisible(s_cursor);
        if (phys >= 0) {
            float textW = (float)Font_MeasureText(s_lines[phys], FONT_SIZE_SMALL);
            float avail = ED_TEXT_W;                 /* interior text column width */
            float overflow = (textW > avail) ? (textW - avail) : 0.0f;
            if (pressed & BTN_DPAD_RIGHT) s_panX += ED_PAN_STEP;
            if (pressed & BTN_DPAD_LEFT)  s_panX -= ED_PAN_STEP;
            if (s_panX < 0.0f)       s_panX = 0.0f;
            if (s_panX > overflow)   s_panX = overflow;
            if (overflow > 0.0f) Audio_PlaySfx(SFX_NAV_UP);
        }
    }

    /* A -> edit highlighted line via the themed OSK, pre-filled */
    if (pressed & BTN_A) {
        int phys = PhysFromVisible(s_cursor);
        if (phys >= 0) {
            s_editingLine = phys;
            Osk_Open(OSK_TEXT, s_lines[phys], FE_MAX_LINE_LEN - 1);
            Audio_PlaySfx(SFX_SELECT);
        }
    }

    /* WHITE -> insert line after; BLACK -> delete line */
    if (pressed & BTN_WHITE) {
        int phys = PhysFromVisible(s_cursor);
        if (phys >= 0) {
            InsertLineAfter(phys);
            s_cursor++; ClampScroll();
            Audio_PlaySfx(SFX_ALT);
        }
    }
    if (pressed & BTN_BLACK) {
        int phys = PhysFromVisible(s_cursor);
        if (phys >= 0) {
            DeleteLine(phys);
            ClampScroll();
            Audio_PlaySfx(SFX_BACK);
        }
    }

    /* START -> save */
    if (pressed & BTN_START) {
        s_msg = SaveFile() ? 1 : 2;
        Audio_PlaySfx(s_msg == 1 ? SFX_SELECT : SFX_BACK);
    }

    /* B -> exit (confirm if dirty) */
    if (pressed & BTN_B) {
        if (s_dirty) { s_confirmExit = 1; Audio_PlaySfx(SFX_ALT); }
        else { Edit_Close(); return 1; }
    }

    return 0;
}

/* ---- draw --------------------------------------------------------------- */

void Edit_Draw(IDirect3DDevice8* d) {
    const Texture* frame;
    DWORD text, dim, accent, glow;
    /* 40px insets: the frame_menu_v chrome is ~35px thick stretched to 560 wide,
       so the old 24px inset let the text, line numbers and the selection bar
       bleed under the metal. 40 clears it with a small margin. */
    float inL = ED_FX + 40.0f, inR = ED_FX + ED_FW - 40.0f;
    float rowY0 = ED_FY + ED_ROW_TOP;
    int vis, live, i, phys;

    if (!s_open) return;

    text = Theme_Color("text", 0xFFD8F8C0);
    dim = Theme_Color("text_dim", 0xFF7FA060);
    accent = Theme_Color("accent", 0xFF7FE000);
    glow = Theme_Color("glow", 0xFFAEFF3C);
    frame = Theme_Asset("frame_menu_v");

    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(170, 0, 0, 0));
    if (frame) UI_DrawSprite(frame, ED_FX, ED_FY, ED_FW, ED_FH, 0xFFFFFFFF, 0);

    /* title bar: filename + dirty marker */
    {
        char hdr[ED_PATH_MAX + 4];
        int n = 0, k = 0, last = -1;
        while (s_path[k]) { if (s_path[k] == '\\') last = k; k++; }   /* basename */
        for (k = last + 1; s_path[k] && n < (int)sizeof(hdr) - 3; k++) hdr[n++] = s_path[k];
        if (s_dirty && n < (int)sizeof(hdr) - 2) { hdr[n++] = ' '; hdr[n++] = '*'; }
        hdr[n] = 0;
        Font_DrawText(d, inL, ED_FY + 34.0f, hdr, FONT_SIZE_MEDIUM, accent, (int)(inR - inL));
    }

    /* line list */
    vis = VisRows();
    live = LiveCount();
    ClampScroll();
    for (i = 0; i < vis; i++) {
        int visIdx = s_scroll + i;
        float ry = rowY0 + ED_ROW_DY * (float)i;
        char num[8];
        DWORD col;
        if (visIdx >= live) break;
        phys = PhysFromVisible(visIdx);
        if (phys < 0) break;

        if (visIdx == s_cursor)
            UI_FillRect(inL, ry - 2.0f, inR - inL, ED_ROW_DY,
                UI_ARGB(70, 174, 255, 60));

        /* line number (dim) + content (clipped to interior) */
        {
            int v = visIdx + 1, p = 0;
            if (v >= 1000) num[p++] = (char)('0' + (v / 1000) % 10);
            if (v >= 100)  num[p++] = (char)('0' + (v / 100) % 10);
            if (v >= 10)   num[p++] = (char)('0' + (v / 10) % 10);
            num[p++] = (char)('0' + v % 10);
            num[p] = 0;
        }
        Font_DrawText(d, inL, ry, num, FONT_SIZE_SMALL, dim, 0);
        col = (visIdx == s_cursor) ? glow : text;
        if (visIdx == s_cursor && s_panX > 0.0f) {
            /* selected line is panned: draw shifted left by s_panX, viewport-
               clipped to the text column so it can't bleed into the gutter or
               past the frame. All virtual coords -> scaled for the clip. */
            float tx = inL + ED_LINENO_W;
            float tw = inR - tx;
            D3DVIEWPORT8 vpOld, vpClip;
            d->GetViewport(&vpOld);
            vpClip.X = (DWORD)UI_Sx(tx);  vpClip.Y = (DWORD)UI_Sy(ry - 2.0f);
            vpClip.Width = (DWORD)UI_ScaleX(tw);
            vpClip.Height = (DWORD)UI_ScaleY(ED_ROW_DY + 2.0f);
            vpClip.MinZ = 0.0f; vpClip.MaxZ = 1.0f;
            d->SetViewport(&vpClip);
            Font_DrawText(d, tx - s_panX, ry, s_lines[phys], FONT_SIZE_SMALL, col, 0);
            d->SetViewport(&vpOld);
        }
        else {
            Font_DrawText(d, inL + ED_LINENO_W, ry, s_lines[phys], FONT_SIZE_SMALL, col,
                (int)(inR - (inL + ED_LINENO_W)));
        }
    }

    /* footer hint / status */
    {
        const char* hint = "A EDIT  L/R PAN  WHITE +LINE  BLACK -LINE  START SAVE  B EXIT";
        DWORD c = dim;
        if (s_msg == 1) { hint = "Saved"; c = accent; }
        else if (s_msg == 2) { hint = "Save failed"; c = Theme_Color("text_dim", 0xFF7FA060); }
        Font_DrawTextCentered(d, inL, ED_FY + ED_FH - 26.0f, inR - inL, hint, FONT_SIZE_SMALL, c);
    }

    /* exit-confirm dialog */
    if (s_confirmExit) {
        float bw = 320.0f, bh = 96.0f;
        float bx = 320.0f - bw * 0.5f, by = 240.0f - bh * 0.5f;
        UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(140, 0, 0, 0));
        UI_FillRect(bx, by, bw, bh, UI_ARGB(235, 12, 22, 12));
        Font_DrawTextCentered(d, 320.0f, by + 20.0f, bw, "Save changes before exit?", FONT_SIZE_SMALL, text);
        Font_DrawTextCentered(d, 320.0f, by + 54.0f, bw, "A = Save   B = Discard   BACK = Cancel", FONT_SIZE_SMALL, dim);
    }

    /* the OSK draws on top when active */
    Osk_Draw(d);
}