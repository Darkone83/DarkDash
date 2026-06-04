/*---------------------------------------------------------------------------
    dd_browse.cpp -- see dd_browse.h.

    Single-pane folder browser. Lists drives at the root, folders within a
    directory (files are shown dimmed but can't be entered/selected). Reuses
    the themed frame + font + UI primitives so it matches the rest of the dash.
    Self-contained listing via FindFirstFile, so it doesn't entangle FileMan.
    C89 style, file-scope statics, no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_browse.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "font.h"
#include "input.h"
#include "dd_audio.h"
#include "dd_fileops.h"   /* Mu_Present / Mu_Letter */
#include "dd_disc.h"      /* Disc_Get               */

#define BR_PATH_MAX     256
#define BR_MAX_ENTRIES  512
#define BR_NAME_MAX     80

typedef struct {
    char name[BR_NAME_MAX];
    char devPath[16];     /* drives only: real root ("E:\") */
    int  isDir;
    int  isDrive;
} BrEntry;

static int      s_open = 0;
static char     s_title[40];
static char     s_path[BR_PATH_MAX];          /* "" = drive list */
static BrEntry  s_ent[BR_MAX_ENTRIES];
static int      s_count = 0, s_cursor = 0, s_scroll = 0;
static char     s_result[BR_PATH_MAX];

#define BR_FX   170.0f
#define BR_FY   60.0f
#define BR_FW   300.0f
#define BR_FH   360.0f
#define BR_VIS_MAX 14

/* ---- tiny string helpers ---------------------------------------------- */

static int  BLen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void BCopy(char* d, int cap, const char* s) {
    int i = 0; if (cap <= 0) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int BNameCmp(const char* a, const char* b) {
    int i = 0;
    for (;;) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return (ca < cb) ? -1 : 1;
        if (ca == 0) return 0;
        i++;
    }
}

/* ---- listing ----------------------------------------------------------- */

static void AddEnt(const char* name, int isDir, int isDrive, const char* dev) {
    BrEntry* e;
    if (s_count >= BR_MAX_ENTRIES) return;
    e = &s_ent[s_count++];
    BCopy(e->name, sizeof(e->name), name);
    e->isDir = isDir; e->isDrive = isDrive;
    e->devPath[0] = 0;
    if (dev) BCopy(e->devPath, sizeof(e->devPath), dev);
}

static void SortEnts(void) {
    int i, j;
    for (i = 1; i < s_count; i++) {
        BrEntry tmp = s_ent[i];
        j = i - 1;
        while (j >= 0) {
            BrEntry* e = &s_ent[j];
            int after;
            if (e->isDir != tmp.isDir) after = (e->isDir && !tmp.isDir);
            else                       after = (BNameCmp(e->name, tmp.name) <= 0);
            if (after) break;
            s_ent[j + 1] = s_ent[j];
            j--;
        }
        s_ent[j + 1] = tmp;
    }
}

static void LoadDrives(void) {
    static const char* const k_hdd[] = { "C", "E", "F", "G", "X", "Y", "Z", 0 };
    int di, mu;
    s_count = 0; s_cursor = 0; s_scroll = 0; s_path[0] = 0;

    for (di = 0; k_hdd[di]; di++) {
        char root[8], label[8];
        DWORD attr;
        root[0] = k_hdd[di][0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
        attr = GetFileAttributesA(root);
        if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            label[0] = k_hdd[di][0]; label[1] = ':'; label[2] = 0;
            AddEnt(label, 1, 1, root);
        }
    }
    for (mu = 0; mu < 8; mu++) {
        int port = mu / 2, slot = mu % 2;
        char root[8], label[8];
        DWORD attr;
        if (!Mu_Present(port, slot)) continue;
        root[0] = Mu_Letter(port, slot); root[1] = ':'; root[2] = '\\'; root[3] = 0;
        attr = GetFileAttributesA(root);
        if (attr == 0xFFFFFFFF || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        label[0] = Mu_Letter(port, slot); label[1] = ':'; label[2] = 0;
        AddEnt(label, 1, 1, root);
    }
    {
        const DiscState* ds = Disc_Get();
        if (ds->present) {
            DWORD attr = GetFileAttributesA("S:\\");
            if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY))
                AddEnt("D: (Disc)", 1, 1, "S:\\");
        }
    }
    SortEnts();   /* order by label so the disc (D:) sits between C: and E: */
}

static void LoadDir(const char* path) {
    char pat[BR_PATH_MAX + 4];
    WIN32_FIND_DATA fd;
    HANDLE h;
    int n;

    if (path[0] == 0) { LoadDrives(); return; }

    BCopy(s_path, sizeof(s_path), path);
    s_count = 0; s_cursor = 0; s_scroll = 0;

    BCopy(pat, sizeof(pat), path);
    n = BLen(pat);
    if (n > 0 && pat[n - 1] != '\\' && n < (int)sizeof(pat) - 2) pat[n++] = '\\';
    pat[n++] = '*'; pat[n] = 0;

    h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int isDir;
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == 0 || fd.cFileName[1] == '.')) continue;
            isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
            AddEnt(fd.cFileName, isDir, 0, 0);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    SortEnts();
}

/* full path of the cursor entry into out */
static void EntryPath(int idx, char* out, int cap) {
    const BrEntry* e;
    int n;
    if (idx < 0 || idx >= s_count) { out[0] = 0; return; }
    e = &s_ent[idx];
    if (e->isDrive) { BCopy(out, cap, e->devPath); return; }
    BCopy(out, cap, s_path);
    n = BLen(out);
    if (n > 0 && out[n - 1] != '\\' && n < cap - 1) { out[n++] = '\\'; out[n] = 0; }
    {
        int i = 0;
        while (e->name[i] && n < cap - 1) out[n++] = e->name[i++];
        out[n] = 0;
    }
}

/* go up one level from s_path; back to drive list if at a drive root */
static void GoUp(void) {
    int n = BLen(s_path), i, lastBs = -1;
    if (n == 0) return;                /* already at drive list */
    /* "E:\" (root) -> drive list */
    if (n <= 3) { LoadDrives(); return; }
    /* strip trailing backslash */
    if (s_path[n - 1] == '\\') { s_path[n - 1] = 0; n--; }
    for (i = 0; i < n; i++) if (s_path[i] == '\\') lastBs = i;
    if (lastBs <= 2) {                 /* parent is the drive root "E:\" */
        char root[8];
        root[0] = s_path[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
        LoadDir(root);
    }
    else {
        char parent[BR_PATH_MAX];
        BCopy(parent, sizeof(parent), s_path);
        parent[lastBs] = 0;
        LoadDir(parent);
    }
}

static void ClampScroll(int vis) {
    if (s_cursor < s_scroll) s_scroll = s_cursor;
    if (s_cursor >= s_scroll + vis) s_scroll = s_cursor - vis + 1;
    if (s_scroll < 0) s_scroll = 0;
}

/* Rows live between the path header and the footer hint. Compute the count
   that fully fits in THAT band at the current font's line height, so the last
   visible row's text never grazes the footer or the frame edge. Must match the
   render's rowY0 / footer placement below. The frame_menu_v asset has a thick
   decorative top border, so content starts well below the frame top (the
   launcher uses the same frame and insets ~38px). */
#define BR_ROW_TOP   82.0f                 /* rowY0 offset from frame top      */
#define BR_FOOTER_H  34.0f                 /* space reserved for footer hint   */

static int VisRows(void) {
    float rowDY = (float)Font_LineHeight(FONT_SIZE_SMALL);
    float avail = BR_FH - BR_ROW_TOP - BR_FOOTER_H;
    int   n;
    if (rowDY < 21.0f) rowDY = 21.0f;
    n = (int)(avail / rowDY);
    if (n < 1) n = 1;
    if (n > BR_VIS_MAX) n = BR_VIS_MAX;
    return n;
}

/* ---- public ------------------------------------------------------------ */

void Browse_Open(const char* title) {
    BCopy(s_title, sizeof(s_title), title ? title : "Select Folder");
    s_result[0] = 0;
    LoadDrives();
    s_open = 1;
}

void Browse_Close(void) { s_open = 0; }
int  Browse_IsOpen(void) { return s_open; }

void Browse_GetPath(char* buf, int buflen) { BCopy(buf, buflen, s_result); }

int Browse_Update(WORD pressed) {
    int vis;
    if (!s_open) return -1;
    vis = VisRows();

    if (pressed & BTN_DPAD_DOWN) {
        if (s_cursor < s_count - 1) { s_cursor++; Audio_PlaySfx(SFX_NAV_DOWN); ClampScroll(vis); }
    }
    if (pressed & BTN_DPAD_UP) {
        if (s_cursor > 0) { s_cursor--; Audio_PlaySfx(SFX_NAV_UP); ClampScroll(vis); }
    }

    /* A: enter the highlighted folder / drive */
    if (pressed & BTN_A) {
        if (s_count > 0 && s_ent[s_cursor].isDir) {
            char np[BR_PATH_MAX];
            EntryPath(s_cursor, np, sizeof(np));
            Audio_PlaySfx(SFX_SELECT);
            LoadDir(np);
        }
    }

    /* B: up a level, or cancel when already at the drive list */
    if (pressed & BTN_B) {
        if (s_path[0] == 0) { Audio_PlaySfx(SFX_BACK); s_open = 0; return -1; }
        Audio_PlaySfx(SFX_BACK);
        GoUp();
    }

    /* Y / START: select the CURRENT folder (the one we're viewing). Only valid
       once we're inside a drive (not at the bare drive list). */
    if ((pressed & BTN_Y) || (pressed & BTN_START)) {
        if (s_path[0] != 0) {
            BCopy(s_result, sizeof(s_result), s_path);
            Audio_PlaySfx(SFX_SELECT);
            s_open = 0;
            return 1;
        }
    }

    /* Back: cancel outright */
    if (pressed & BTN_BACK) { Audio_PlaySfx(SFX_BACK); s_open = 0; return -1; }

    return 0;
}

void Browse_Draw(IDirect3DDevice8* d) {
    const Texture* frame;
    DWORD text, glow, dim, accent;
    float rowY0, rowDY, hy;
    int   i, vis;

    if (!s_open) return;

    text = Theme_Color("text", 0xFFD8F8C0);
    glow = Theme_Color("glow", 0xFFAEFF3C);
    accent = Theme_Color("accent", 0xFF7FE000);
    dim = UI_ARGB(150, 200, 220, 190);
    frame = Theme_Asset("frame_menu_v");

    /* dim the screen behind, then the framed picker */
    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(150, 0, 0, 0));
    if (frame) UI_DrawSprite(frame, BR_FX, BR_FY, BR_FW, BR_FH, 0xFFFFFFFF, 0);

    /* title + current path header -- inset below the frame's top chrome */
    Font_DrawText(d, BR_FX + 22.0f, BR_FY + 34.0f, s_title, FONT_SIZE_MEDIUM, accent, 0);
    {
        const char* p = s_path[0] ? s_path : "/  (drives)";
        Font_DrawText(d, BR_FX + 22.0f, BR_FY + 60.0f, p, FONT_SIZE_SMALL, dim,
            (int)(BR_FW - 44.0f));
    }

    rowY0 = BR_FY + BR_ROW_TOP;
    rowDY = (float)Font_LineHeight(FONT_SIZE_SMALL);
    if (rowDY < 21.0f) rowDY = 21.0f;
    vis = VisRows();

    hy = 0.0f; (void)hy;
    {
        int shown = s_count - s_scroll;
        if (shown > vis) shown = vis;
        for (i = 0; i < shown; i++) {
            int   idx = s_scroll + i;
            const BrEntry* e = &s_ent[idx];
            float ry = rowY0 + rowDY * (float)i;
            DWORD c;
            if (idx == s_cursor) {
                float gh = (float)Font_GlyphHeight(FONT_SIZE_SMALL) + 4.0f;
                if (gh > rowDY) gh = rowDY;
                UI_FillRect(BR_FX + 14.0f, ry + (rowDY - gh) * 0.5f - 1.0f,
                    BR_FW - 28.0f, gh, UI_ARGB(60, 174, 255, 60));
            }
            /* folders/drives in normal text, files dimmed (not selectable) */
            c = e->isDir ? ((idx == s_cursor) ? glow : text)
                : UI_ARGB(110, 200, 220, 190);
            Font_DrawText(d, BR_FX + 24.0f, ry, e->name, FONT_SIZE_SMALL, c,
                (int)(BR_FW - 44.0f));
        }
    }

    /* footer hint (sits in the reserved BR_FOOTER_H band, clear of bottom chrome) */
    Font_DrawText(d, BR_FX + 22.0f, BR_FY + BR_FH - 26.0f,
        "A OPEN  Y SELECT  B UP", FONT_SIZE_SMALL, dim, 0);
}