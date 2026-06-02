/*---------------------------------------------------------------------------
    FileMan.cpp -- dual-pane FILE MANAGER (stage 1: navigation scaffold).

    Two flat panes (source left, destination right), each a Pane holding a
    current path + entry list + cursor/scroll + marks. One pane is active.
    Directory listing is done here (pane-state-specific) via FindFirstFile;
    the virtual root "/" lists drives + present MUs. Operations (copy/move/
    delete/rename/mkdir) layer on in stage 2 via dd_fileops.

    Build: MSVC2003/C89 style -- declarations precede statements, file-scope
    statics, no sprintf/strlen.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "font.h"
#include "input.h"
#include "dd_audio.h"
#include "dd_backdrop.h"
#include "dd_fileops.h"
#include "dd_osk.h"
#include "dd_mount.h"
#include "FileMan.h"

#define FM_PATH_MAX     256
#define FM_NAME_MAX      64
#define FM_MAX_ENTRIES  512
#define FM_VIS_ROWS      14     /* rows shown per pane */

typedef struct {
    char name[FM_NAME_MAX];
    int  isDir;
    int  isDrive;          /* a virtual-root drive/MU entry */
    char devPath[FM_PATH_MAX];   /* for drives: the real root ("E:\") */
    DWORD sizeLo;
    int  marked;
} FmEntry;

typedef struct {
    char     path[FM_PATH_MAX];      /* "" = virtual root (drive list) */
    FmEntry  ent[FM_MAX_ENTRIES];
    int      count;
    int      cursor;
    int      scroll;
} Pane;

static Pane s_pane[2];        /* 0 = source (left), 1 = dest (right) */
static int  s_active = 0;     /* which pane has focus */

/* ---- stage-2 modes / overlays / dialogs -------------------------------- */

enum {
    FM_BROWSE = 0,    /* normal dual-pane navigation             */
    FM_OPS,           /* ops menu overlay open                   */
    FM_DESTPICK,      /* choosing a paste destination (copy/move)*/
    FM_CONFIRM_DEL,   /* "delete N items?" dialog                */
    FM_CONFIRM_EXIT,  /* "exit file manager?" dialog             */
    FM_OSK_RENAME,    /* OSK open, renaming the highlighted item */
    FM_OSK_MKDIR      /* OSK open, naming a new folder           */
};
static int s_mode = FM_BROWSE;

/* ops menu */
enum { OP_COPY = 0, OP_MOVE, OP_DELETE, OP_RENAME, OP_MKDIR, OP_COUNT };
static const char* k_opNames[OP_COUNT] = {
    "Copy", "Move", "Delete", "Rename", "New Folder"
};
static int s_opCursor = 0;

/* a pending copy/move: which pane is the source, and is it a move? */
static int s_pendMove = 0;     /* 0 = copy, 1 = move */
static int s_pendSrc = 0;     /* source pane index   */

/* transient status message (e.g. "Copied", "Delete failed") */
static char s_msg[48] = { 0 };
static void SetMsg(const char* m) {
    int i = 0; while (m[i] && i < (int)sizeof(s_msg) - 1) { s_msg[i] = m[i]; i++; } s_msg[i] = 0;
}

/* ---- small helpers (no CRT str*) --------------------------------------- */

static int FmLen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void FmCopy(char* dst, int cap, const char* src) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void FmJoin(char* path, int cap, const char* name) {
    int n = FmLen(path), i = 0;
    if (n > 0 && path[n - 1] != '\\' && n < cap - 1) path[n++] = '\\';
    while (name[i] && n < cap - 1) path[n++] = name[i++];
    path[n] = 0;
}

/* ---- directory / drive-root listing ------------------------------------ */

static void AddEntry(Pane* p, const char* name, int isDir, int isDrive,
    const char* devPath, DWORD sizeLo) {
    FmEntry* e;
    if (p->count >= FM_MAX_ENTRIES) return;
    e = &p->ent[p->count++];
    FmCopy(e->name, sizeof(e->name), name);
    e->isDir = isDir; e->isDrive = isDrive;
    e->devPath[0] = 0;
    if (devPath) FmCopy(e->devPath, sizeof(e->devPath), devPath);
    e->sizeLo = sizeLo;
    e->marked = 0;
}

/* virtual root: list mounted HDD drives + present MUs */
static void LoadDriveList(Pane* p) {
    static const char* k_hdd[] = { "C", "E", "F", "G", "X", "Y", "Z", 0 };
    int mu, di;
    p->count = 0; p->cursor = 0; p->scroll = 0;
    p->path[0] = 0;

    for (di = 0; k_hdd[di]; di++) {
        char root[8];
        DWORD attr;
        root[0] = k_hdd[di][0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
        attr = GetFileAttributesA(root);
        if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            char label[8];
            label[0] = k_hdd[di][0]; label[1] = ':'; label[2] = 0;
            AddEntry(p, label, 1, 1, root, 0);
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
        AddEntry(p, label, 1, 1, root, 0);
    }
}

static void LoadDirectory(Pane* p, const char* path) {
    char pat[FM_PATH_MAX + 4];
    WIN32_FIND_DATA fd;
    HANDLE h;
    int n;

    if (path[0] == 0) { LoadDriveList(p); return; }

    FmCopy(p->path, sizeof(p->path), path);
    p->count = 0; p->cursor = 0; p->scroll = 0;

    FmCopy(pat, sizeof(pat), path);
    n = FmLen(pat);
    if (n > 0 && pat[n - 1] != '\\' && n < (int)sizeof(pat) - 2) pat[n++] = '\\';
    pat[n++] = '*'; pat[n] = 0;

    h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int isDir;
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == 0 || fd.cFileName[1] == '.')) continue;
            isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
            AddEntry(p, fd.cFileName, isDir, 0, 0, fd.nFileSizeLow);
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
}

/* build the full path of the cursor entry into out */
static void EntryPath(const Pane* p, int idx, char* out, int cap) {
    const FmEntry* e;
    if (idx < 0 || idx >= p->count) { out[0] = 0; return; }
    e = &p->ent[idx];
    if (e->isDrive) { FmCopy(out, cap, e->devPath); return; }
    FmCopy(out, cap, p->path);
    FmJoin(out, cap, e->name);
}

/* ---- navigation -------------------------------------------------------- */

/* case-insensitive ".xbe" suffix test */
static int IsXbe(const char* name) {
    int n = 0; while (name[n]) n++;
    if (n < 4) return 0;
    {
        const char* e = name + n - 4;
        char a = e[1], b = e[2], c = e[3];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        return (e[0] == '.' && a == 'x' && b == 'b' && c == 'e');
    }
}

static void EnterEntry(Pane* p) {
    char np[FM_PATH_MAX];
    const FmEntry* e;
    if (p->cursor < 0 || p->cursor >= p->count) return;
    e = &p->ent[p->cursor];
    if (e->isDir) {
        EntryPath(p, p->cursor, np, sizeof(np));
        LoadDirectory(p, np);
        return;
    }
    /* a file: launch it if it's an .xbe (does not return on success) */
    if (IsXbe(e->name)) {
        EntryPath(p, p->cursor, np, sizeof(np));
        Mount_LaunchXbe(np);               /* returns only on failure */
        SetMsg("Launch failed");
    }
}

/* go up a directory; returns 1 if it moved, 0 if already at virtual root */
static int UpDir(Pane* p) {
    int n, lastBs;
    if (p->path[0] == 0) return 0;         /* already at drive list */
    n = FmLen(p->path);
    /* "E:\" -> root listing means go to virtual root */
    if (n <= 3) { LoadDriveList(p); return 1; }
    lastBs = -1;
    { int i; for (i = 0; i < n; i++) if (p->path[i] == '\\') lastBs = i; }
    if (lastBs <= 2) {
        /* parent is the drive root "E:\" */
        char root[8];
        root[0] = p->path[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
        LoadDirectory(p, root);
    }
    else {
        char parent[FM_PATH_MAX];
        FmCopy(parent, sizeof(parent), p->path);
        parent[lastBs] = 0;
        LoadDirectory(p, parent);
    }
    return 1;
}

static void MoveCursor(Pane* p, int dir) {
    if (p->count == 0) return;
    p->cursor += dir;
    if (p->cursor < 0) p->cursor = 0;
    if (p->cursor >= p->count) p->cursor = p->count - 1;
    if (p->cursor < p->scroll) p->scroll = p->cursor;
    if (p->cursor >= p->scroll + FM_VIS_ROWS) p->scroll = p->cursor - FM_VIS_ROWS + 1;
}

/* ---- stage-2 operation helpers ----------------------------------------- */

/* count marked entries in a pane (drives never count) */
static int MarkedCount(const Pane* p) {
    int i, n = 0;
    for (i = 0; i < p->count; i++)
        if (p->ent[i].marked && !p->ent[i].isDrive) n++;
    return n;
}

/* reload a pane at its current path, preserving cursor where possible */
static void ReloadPane(Pane* p) {
    int cur = p->cursor, scr = p->scroll;
    char path[FM_PATH_MAX];
    FmCopy(path, sizeof(path), p->path);
    LoadDirectory(p, path);            /* clears cursor/scroll */
    if (cur >= p->count) cur = p->count - 1;
    if (cur < 0) cur = 0;
    p->cursor = cur;
    p->scroll = (scr <= cur) ? scr : 0;
    if (p->cursor >= p->scroll + FM_VIS_ROWS) p->scroll = p->cursor - FM_VIS_ROWS + 1;
}

/* run copy or move of the source pane's marked (or highlighted) items into
   destDir. Returns count succeeded. */
static int RunPaste(int srcPane, const char* destDir, int isMove) {
    Pane* s = &s_pane[srcPane];
    int i, ok = 0;
    int useMarks = (MarkedCount(s) > 0);

    for (i = 0; i < s->count; i++) {
        FmEntry* e = &s->ent[i];
        char src[FM_PATH_MAX], dst[FM_PATH_MAX];
        int r;
        if (e->isDrive) continue;
        if (useMarks) { if (!e->marked) continue; }
        else { if (i != s->cursor) continue; }

        FmCopy(src, sizeof(src), s->path);
        FmJoin(src, sizeof(src), e->name);
        FmCopy(dst, sizeof(dst), destDir);
        FmJoin(dst, sizeof(dst), e->name);

        if (isMove)            r = Fileops_Move(src, dst);
        else if (e->isDir)     r = Fileops_CopyTree(src, dst);
        else                   r = Fileops_CopyFile(src, dst);
        if (r) { ok++; e->marked = 0; }
    }
    return ok;
}

/* delete the active pane's marked (or highlighted) items. Returns count done. */
static int RunDelete(Pane* p) {
    int i, ok = 0;
    int useMarks = (MarkedCount(p) > 0);
    for (i = 0; i < p->count; i++) {
        FmEntry* e = &p->ent[i];
        char path[FM_PATH_MAX];
        if (e->isDrive) continue;
        if (useMarks) { if (!e->marked) continue; }
        else { if (i != p->cursor) continue; }
        FmCopy(path, sizeof(path), p->path);
        FmJoin(path, sizeof(path), e->name);
        if (Fileops_Delete(path)) ok++;
    }
    return ok;
}

/* ---- lifecycle --------------------------------------------------------- */

void FileMan_Enter(void) {
    Mu_MountAll();                         /* ensure drives + MUs are bound */
    LoadDriveList(&s_pane[0]);
    LoadDriveList(&s_pane[1]);
    s_active = 0;
    s_mode = FM_BROWSE;
    s_msg[0] = 0;
}

int FileMan_Update(WORD pressed, WORD held) {
    Pane* p = &s_pane[s_active];
    (void)held;

    /* OSK modes: let the on-screen keyboard consume input */
    if (s_mode == FM_OSK_RENAME || s_mode == FM_OSK_MKDIR) {
        int r = Osk_Update(pressed);
        if (r == 1) {                       /* confirm */
            char name[FM_NAME_MAX];
            Osk_GetText(name, sizeof(name));
            Osk_Close();
            if (name[0]) {
                if (s_mode == FM_OSK_MKDIR) {
                    char np[FM_PATH_MAX];
                    FmCopy(np, sizeof(np), p->path);
                    FmJoin(np, sizeof(np), name);
                    SetMsg(Fileops_MkDir(np) ? "Folder created" : "Create failed");
                }
                else {                    /* rename highlighted */
                    if (p->cursor >= 0 && p->cursor < p->count && !p->ent[p->cursor].isDrive) {
                        char oldp[FM_PATH_MAX], newp[FM_PATH_MAX];
                        FmCopy(oldp, sizeof(oldp), p->path);
                        FmJoin(oldp, sizeof(oldp), p->ent[p->cursor].name);
                        FmCopy(newp, sizeof(newp), p->path);
                        FmJoin(newp, sizeof(newp), name);
                        SetMsg(Fileops_Rename(oldp, newp) ? "Renamed" : "Rename failed");
                    }
                }
                ReloadPane(p);
            }
            s_mode = FM_BROWSE;
        }
        else if (r == -1) {               /* cancel */
            Osk_Close();
            s_mode = FM_BROWSE;
        }
        return 0;
    }

    /* delete confirm dialog */
    if (s_mode == FM_CONFIRM_DEL) {
        if (pressed & BTN_A) {
            int n = RunDelete(p);
            SetMsg(n > 0 ? "Deleted" : "Delete failed");
            ReloadPane(p);
            s_mode = FM_BROWSE;
            Audio_PlaySfx(SFX_SELECT);
        }
        else if (pressed & BTN_B) {
            s_mode = FM_BROWSE;
            Audio_PlaySfx(SFX_BACK);
        }
        return 0;
    }

    /* exit confirm dialog */
    if (s_mode == FM_CONFIRM_EXIT) {
        if (pressed & BTN_A) { s_mode = FM_BROWSE; return 1; }   /* leave FileMan */
        if (pressed & BTN_B) { s_mode = FM_BROWSE; Audio_PlaySfx(SFX_BACK); }
        return 0;
    }

    /* ops menu overlay */
    if (s_mode == FM_OPS) {
        if (pressed & BTN_DPAD_DOWN) { if (s_opCursor < OP_COUNT - 1) { s_opCursor++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_opCursor > 0) { s_opCursor--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & BTN_B) { s_mode = FM_BROWSE; Audio_PlaySfx(SFX_BACK); return 0; }
        if (pressed & BTN_A) {
            Audio_PlaySfx(SFX_SELECT);
            switch (s_opCursor) {
            case OP_COPY:
            case OP_MOVE:
                s_pendMove = (s_opCursor == OP_MOVE);
                s_pendSrc = s_active;
                s_active = (s_active == 0) ? 1 : 0;   /* focus dest pane */
                s_mode = FM_DESTPICK;
                SetMsg(s_pendMove ? "Move: pick dest, WHITE=paste" : "Copy: pick dest, WHITE=paste");
                break;
            case OP_DELETE:
                s_mode = FM_CONFIRM_DEL;
                break;
            case OP_RENAME:
                if (p->cursor >= 0 && p->cursor < p->count && !p->ent[p->cursor].isDrive) {
                    Osk_Open(OSK_TEXT, p->ent[p->cursor].name, FM_NAME_MAX - 1);
                    s_mode = FM_OSK_RENAME;
                }
                else s_mode = FM_BROWSE;
                break;
            case OP_MKDIR:
                if (p->path[0]) {           /* can't mkdir at the drive list */
                    Osk_Open(OSK_TEXT, "", FM_NAME_MAX - 1);
                    s_mode = FM_OSK_MKDIR;
                }
                else { SetMsg("Open a drive first"); s_mode = FM_BROWSE; }
                break;
            }
        }
        return 0;
    }

    /* destination-pick mode: navigate the dest pane, WHITE pastes, B cancels */
    if (s_mode == FM_DESTPICK) {
        if (pressed & BTN_DPAD_DOWN) { MoveCursor(p, 1); Audio_PlaySfx(SFX_NAV_DOWN); }
        if (pressed & BTN_DPAD_UP) { MoveCursor(p, -1); Audio_PlaySfx(SFX_NAV_UP); }
        if (pressed & BTN_A) { EnterEntry(p); Audio_PlaySfx(SFX_SELECT); }   /* navigate into folders */
        if (pressed & BTN_X) { if (UpDir(p)) Audio_PlaySfx(SFX_BACK); }
        if (pressed & BTN_WHITE) {
            if (p->path[0]) {               /* must be inside a real folder */
                int n = RunPaste(s_pendSrc, p->path, s_pendMove);
                SetMsg(n > 0 ? (s_pendMove ? "Moved" : "Copied") : "Operation failed");
                ReloadPane(&s_pane[s_pendSrc]);
                ReloadPane(p);
                s_mode = FM_BROWSE;
                Audio_PlaySfx(SFX_SELECT);
            }
            else {
                SetMsg("Pick a folder first");
            }
        }
        if (pressed & BTN_B) {              /* cancel the paste */
            s_active = s_pendSrc;
            s_mode = FM_BROWSE;
            SetMsg("Cancelled");
            Audio_PlaySfx(SFX_BACK);
        }
        return 0;
    }

    /* ---- FM_BROWSE: normal dual-pane navigation ---- */
    if (pressed & BTN_LTRIG) { if (s_active != 0) { s_active = 0; Audio_PlaySfx(SFX_ALT); } }
    if (pressed & BTN_RTRIG) { if (s_active != 1) { s_active = 1; Audio_PlaySfx(SFX_ALT); } }

    if (pressed & BTN_DPAD_DOWN) { MoveCursor(p, 1); Audio_PlaySfx(SFX_NAV_DOWN); }
    if (pressed & BTN_DPAD_UP) { MoveCursor(p, -1); Audio_PlaySfx(SFX_NAV_UP); }

    if (pressed & BTN_A) { EnterEntry(p); Audio_PlaySfx(SFX_SELECT); }
    if (pressed & BTN_X) { if (UpDir(p)) Audio_PlaySfx(SFX_BACK); }

    if (pressed & BTN_Y) {                 /* mark/unmark */
        if (p->cursor >= 0 && p->cursor < p->count && !p->ent[p->cursor].isDrive) {
            p->ent[p->cursor].marked ^= 1;
            Audio_PlaySfx(SFX_ALT);
        }
    }

    if (pressed & BTN_BLACK) {             /* open ops menu */
        s_opCursor = 0;
        s_mode = FM_OPS;
        Audio_PlaySfx(SFX_SELECT);
    }

    if (pressed & BTN_B) {                 /* cancel/exit */
        if (p->path[0] == 0) {             /* at drive-list root -> confirm exit */
            s_mode = FM_CONFIRM_EXIT;
            Audio_PlaySfx(SFX_SELECT);
        }
        else {                           /* otherwise back out one level */
            UpDir(p);
            Audio_PlaySfx(SFX_BACK);
        }
    }
    return 0;
}

/* ---- render ------------------------------------------------------------ */

static void DrawPane(IDirect3DDevice8* d, const Pane* p, int isActive,
    float px, const Texture* hdr, const Texture* frame) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    DWORD mark = Theme_Color("accent", 0xFF7FE000);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    float hy = 40.0f, fy = 86.0f, fw = 284.0f, fh = 360.0f;
    float rowY0 = fy + 16.0f, rowDY = 23.0f;
    int   i, vis;

    /* path-header banner */
    if (hdr) UI_DrawSprite(hdr, px, hy, fw, 36.0f,
        isActive ? 0xFFFFFFFF : UI_ARGB(150, 255, 255, 255), 0);
    {
        const char* label = p->path[0] ? p->path : "/  (drives)";
        Font_DrawText(d, px + 12.0f, hy + 9.0f, label, FONT_SIZE_SMALL,
            isActive ? accent : dim, (int)(fw - 20.0f));
    }

    /* frame (flat; dim if not active) */
    if (frame) UI_DrawSprite(frame, px, fy, fw, fh,
        isActive ? 0xFFFFFFFF : UI_ARGB(140, 255, 255, 255), 0);

    /* rows */
    vis = p->count - p->scroll;
    if (vis > FM_VIS_ROWS) vis = FM_VIS_ROWS;
    for (i = 0; i < vis; i++) {
        int    idx = p->scroll + i;
        const FmEntry* e = &p->ent[idx];
        float  ry = rowY0 + rowDY * (float)i;
        DWORD  c;

        if (isActive && idx == p->cursor)
            UI_FillRect(px + 10.0f, ry - 2.0f, fw - 20.0f, 20.0f,
                UI_ARGB(70, ar, ag, ab));
        if (e->marked)
            UI_FillRect(px + 10.0f, ry - 2.0f, 4.0f, 20.0f, mark);

        c = (isActive && idx == p->cursor) ? glow
            : (e->isDir ? text : dim);
        Font_DrawText(d, px + 22.0f, ry, e->name, FONT_SIZE_SMALL, c,
            (int)(fw - 70.0f));

        /* dir marker / size on the right */
        if (e->isDir)
            Font_DrawTextRight(d, px + fw - 14.0f, ry,
                e->isDrive ? "drive" : "dir", FONT_SIZE_SMALL, dim);
    }

    /* scroll hint */
    if (p->count > FM_VIS_ROWS) {
        char n[8]; int v = p->cursor + 1, k = 0; char tmp[8];
        while (v > 0 && k < 7) { tmp[k++] = (char)('0' + (v % 10)); v /= 10; }
        { int j; for (j = 0; j < k; j++) n[j] = tmp[k - 1 - j]; n[k] = 0; }
        Font_DrawTextRight(d, px + fw - 14.0f, fy + fh - 18.0f, n, FONT_SIZE_SMALL, dim);
    }
}

/* draw a themed, centered framed box (reuses the menu frame) */
static void DrawFramedBox(IDirect3DDevice8* d, float x, float y, float w, float h) {
    const Texture* frame = Theme_Asset("frame_menu_v");
    /* dim the scene behind the dialog */
    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(150, 0, 0, 0));
    if (frame) UI_DrawSprite(frame, x, y, w, h, 0xFFFFFFFF, 0);
    else       UI_FillRect(x, y, w, h, UI_ARGB(235, 18, 22, 18));
}

void FileMan_Render(void) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* hdr = Theme_Asset("bar_header");
    const Texture* frame = Theme_Asset("frame_menu_v");
    const Texture* foot = Theme_Asset("bar_footer");
    const char* footHint;

    Backdrop_Draw();

    /* title + transient status message */
    Font_DrawText(d, 24.0f, 12.0f, "FILE MANAGER", FONT_SIZE_MEDIUM, accent, 0);
    if (s_msg[0])
        Font_DrawTextRight(d, 624.0f, 16.0f, s_msg, FONT_SIZE_SMALL, glow);

    /* two flat panes: source left, dest right. In dest-pick the active pane
       (the destination) gets the focus highlight automatically. */
    DrawPane(d, &s_pane[0], s_active == 0, 20.0f, hdr, frame);
    DrawPane(d, &s_pane[1], s_active == 1, 336.0f, hdr, frame);

    /* ---- overlays / dialogs ---- */
    if (s_mode == FM_OPS) {
        float bx = 232.0f, by = 150.0f, bw = 176.0f, bh = 200.0f;
        int i;
        DrawFramedBox(d, bx, by, bw, bh);
        Font_DrawText(d, bx + 20.0f, by + 16.0f, "OPERATIONS", FONT_SIZE_SMALL, accent, 0);
        for (i = 0; i < OP_COUNT; i++) {
            float ry = by + 44.0f + (float)i * 26.0f;
            if (i == s_opCursor)
                UI_FillRect(bx + 12.0f, ry - 2.0f, bw - 24.0f, 22.0f, UI_ARGB(90, ar, ag, ab));
            Font_DrawText(d, bx + 22.0f, ry, k_opNames[i], FONT_SIZE_SMALL,
                (i == s_opCursor) ? glow : text, 0);
        }
    }
    else if (s_mode == FM_CONFIRM_DEL) {
        float bx = 200.0f, by = 190.0f, bw = 240.0f, bh = 110.0f;
        int n = MarkedCount(&s_pane[s_active]);
        char line[40]; int k = 0; char num[8]; int v, kk;
        DrawFramedBox(d, bx, by, bw, bh);
        Font_DrawText(d, bx + 20.0f, by + 20.0f, "Delete items?", FONT_SIZE_MEDIUM, accent, 0);
        /* "N selected" (or "this item" when nothing marked) */
        if (n > 0) {
            v = n; kk = 0; while (v > 0 && kk < 7) { num[kk++] = (char)('0' + v % 10); v /= 10; }
            { int j; for (j = 0; j < kk; j++) line[k++] = num[kk - 1 - j]; }
            line[k++] = ' '; line[k++] = 's'; line[k++] = 'e'; line[k++] = 'l';
            line[k++] = 'e'; line[k++] = 'c'; line[k++] = 't'; line[k++] = 'e'; line[k++] = 'd'; line[k] = 0;
        }
        else {
            FmCopy(line, sizeof(line), "this item");
        }
        Font_DrawText(d, bx + 20.0f, by + 52.0f, line, FONT_SIZE_SMALL, text, 0);
        Font_DrawText(d, bx + 20.0f, by + 80.0f, "A = yes    B = no", FONT_SIZE_SMALL, dim, 0);
    }
    else if (s_mode == FM_CONFIRM_EXIT) {
        float bx = 200.0f, by = 200.0f, bw = 240.0f, bh = 96.0f;
        DrawFramedBox(d, bx, by, bw, bh);
        Font_DrawText(d, bx + 20.0f, by + 22.0f, "Exit file manager?", FONT_SIZE_MEDIUM, accent, 0);
        Font_DrawText(d, bx + 20.0f, by + 58.0f, "A = yes    B = no", FONT_SIZE_SMALL, dim, 0);
    }

    /* OSK draws on top of everything when open */
    if (s_mode == FM_OSK_RENAME || s_mode == FM_OSK_MKDIR)
        Osk_Draw(d);

    /* ---- mode-aware footer ---- */
    switch (s_mode) {
    case FM_OPS:          footHint = "A SELECT   B CANCEL"; break;
    case FM_DESTPICK:     footHint = "A ENTER  X UP  WHITE PASTE  B CANCEL"; break;
    case FM_CONFIRM_DEL:
    case FM_CONFIRM_EXIT: footHint = "A YES   B NO"; break;
    case FM_OSK_RENAME:
    case FM_OSK_MKDIR:    footHint = ""; break;   /* OSK shows its own */
    default:              footHint = "A ENTER  X UP  Y MARK  LT/RT PANE  BLACK OPS  B BACK"; break;
    }
    if (foot) UI_DrawSprite(foot, 8.0f, 452.0f, 624.0f, 24.0f, 0xFFFFFFFF, 0);
    if (footHint[0])
        Font_DrawText(d, 16.0f, 457.0f, footHint, FONT_SIZE_SMALL, text, 610);
}