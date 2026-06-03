/*---------------------------------------------------------------------------
    dd_savemgr.cpp -- savegame manager (see dd_savemgr.h).

    Flat, dead-simple view: one row per game (E:\UDATA\<TitleID>), friendly name
    from TitleMeta.xbx, and the selected game's TitleImage.xbx art shown beside
    the list (art left, list right). Actions (Copy / Move / Delete) act on the
    WHOLE game folder via the shared Fileops_* routines. Copy/Move use a
    built-in single-pane destination picker (any drive/folder incl. MUs).

    Build: MSVC2003/C89 style -- declarations before statements; file-scope
    statics; no sprintf/strlen (inline helpers).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_savemgr.h"
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "font.h"
#include "input.h"
#include "dd_audio.h"
#include "dd_backdrop.h"
#include "dd_fileops.h"
#include "dd_mount.h"

#define SM_PATH   272
#define SM_NAME    48
#define SM_MAX    256
#define UDATA_ROOT "E:\\UDATA"

enum {
    SM_LIST = 0,    /* flat game list (+ art) */
    SM_OPS,         /* copy/move/delete menu for the selected game */
    SM_DEST,        /* destination picker (copy/move) */
    SM_CONFIRM_DEL  /* delete confirmation */
};

typedef struct {
    char name[SM_NAME];     /* friendly title */
    char folder[SM_NAME];   /* TitleID folder */
} SmEntry;

static int     s_state = SM_LIST;
static SmEntry s_list[SM_MAX];
static int     s_count = 0;
static int     s_cursor = 0;
static int     s_scroll = 0;

static Texture s_art = { 0, 0, 0, 0, 0 };
static int     s_artFor = -1;

static int     s_opCursor = 0;
static int     s_pendMove = 0;

static char    s_destPath[SM_PATH];
static SmEntry s_dest[SM_MAX];
static int     s_destCount = 0;
static int     s_destCursor = 0;
static int     s_destScroll = 0;

static char    s_msg[48] = { 0 };

static const char* const k_opNames[3] = { "Copy", "Move", "Delete" };

/* ---- string helpers ---------------------------------------------------- */

static int SmLen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void SmCopy(char* dst, int cap, const char* src) {
    int i = 0; if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void SmCat(char* dst, int cap, const char* src) {
    int n = SmLen(dst), i = 0;
    while (src && src[i] && n < cap - 1) { dst[n++] = src[i++]; }
    dst[n] = 0;
}

static void SmSetMsg(const char* m) { SmCopy(s_msg, sizeof(s_msg), m); }

static void SmJoin(char* out, int cap, const char* base, const char* leaf) {
    int n;
    SmCopy(out, cap, base);
    n = SmLen(out);
    if (n > 0 && out[n - 1] != '\\' && n < cap - 1) { out[n++] = '\\'; out[n] = 0; }
    SmCat(out, cap, leaf);
}

/* ---- .xbx metadata name parsing ---------------------------------------- */

static int ReadMetaName(const char* metaPath, const char* key, char* out, int cap) {
    HANDLE h;
    BYTE   buf[512];
    DWORD  got = 0;
    int    klen = SmLen(key);
    int    i, j, vi;

    out[0] = 0;
    h = CreateFileA(metaPath, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) || got == 0) { CloseHandle(h); return 0; }
    CloseHandle(h);
    buf[got] = 0;

    for (i = 0; i < (int)got; i++) {
        int p = i, k = 0;
        while (k < klen && p < (int)got) {
            BYTE b = buf[p];
            if (b == 0) { p++; continue; }
            if (b != (BYTE)key[k]) break;
            k++; p++;
        }
        if (k == klen) {
            while (p < (int)got && buf[p] == 0) p++;
            if (p < (int)got && buf[p] == '=') {
                p++;
                vi = 0;
                for (j = p; j < (int)got && vi < cap - 1; j++) {
                    BYTE b = buf[j];
                    if (b == 0) continue;
                    if (b == '\r' || b == '\n') break;
                    out[vi++] = (char)b;
                }
                out[vi] = 0;
                return (vi > 0) ? 1 : 0;
            }
        }
    }
    return 0;
}

/* ---- listing + art ----------------------------------------------------- */

static void AddEntry(SmEntry* arr, int* count, const char* name, const char* folder) {
    if (*count >= SM_MAX) return;
    SmCopy(arr[*count].name, SM_NAME, name);
    SmCopy(arr[*count].folder, SM_NAME, folder);
    (*count)++;
}

static int SmCmp(const char* a, const char* b) {
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

static void SortList(SmEntry* arr, int count) {
    int i, j;
    for (i = 1; i < count; i++) {
        SmEntry tmp = arr[i];
        j = i - 1;
        while (j >= 0 && SmCmp(arr[j].name, tmp.name) > 0) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = tmp;
    }
}

static void FreeArt(void) {
    if (s_art.tex) Texture_Release(&s_art);
    s_art.tex = NULL; s_artFor = -1;
}

static void LoadArt(void) {
    char gdir[SM_PATH], img[SM_PATH];
    if (s_artFor == s_cursor) return;
    FreeArt();
    if (s_count == 0) return;
    SmJoin(gdir, sizeof(gdir), UDATA_ROOT, s_list[s_cursor].folder);
    SmJoin(img, sizeof(img), gdir, "TitleImage.xbx");
    Texture_LoadXPR(img, &s_art);
    s_artFor = s_cursor;
}

static void LoadGames(void) {
    char pat[SM_PATH];
    WIN32_FIND_DATA fd;
    HANDLE h;

    s_count = 0; s_cursor = 0; s_scroll = 0;
    FreeArt();
    SmJoin(pat, sizeof(pat), UDATA_ROOT, "*");
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char meta[SM_PATH], name[SM_NAME], gdir[SM_PATH];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        SmJoin(gdir, sizeof(gdir), UDATA_ROOT, fd.cFileName);
        SmJoin(meta, sizeof(meta), gdir, "TitleMeta.xbx");
        if (!ReadMetaName(meta, "TitleName", name, sizeof(name)))
            SmCopy(name, sizeof(name), fd.cFileName);
        AddEntry(s_list, &s_count, name, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    SortList(s_list, s_count);
    LoadArt();
}

static void LoadDest(const char* path) {
    static const char* k_hdd[] = { "C", "E", "F", "G", "X", "Y", "Z", 0 };
    s_destCount = 0; s_destCursor = 0; s_destScroll = 0;
    SmCopy(s_destPath, sizeof(s_destPath), path ? path : "");

    if (s_destPath[0] == 0) {
        int di, mu;
        for (di = 0; k_hdd[di]; di++) {
            char root[8], label[8];
            DWORD attr;
            root[0] = k_hdd[di][0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
            attr = GetFileAttributesA(root);
            if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                label[0] = k_hdd[di][0]; label[1] = ':'; label[2] = 0;
                AddEntry(s_dest, &s_destCount, label, root);
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
            AddEntry(s_dest, &s_destCount, label, root);
        }
        return;
    }
    {
        char pat[SM_PATH];
        WIN32_FIND_DATA fd;
        HANDLE h;
        SmJoin(pat, sizeof(pat), s_destPath, "*");
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.') continue;
            AddEntry(s_dest, &s_destCount, fd.cFileName, fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        SortList(s_dest, s_destCount);
    }
}

static void DestEnter(void) {
    char np[SM_PATH];
    if (s_destCount == 0) return;
    if (s_destPath[0] == 0) { LoadDest(s_dest[s_destCursor].folder); }
    else { SmJoin(np, sizeof(np), s_destPath, s_dest[s_destCursor].folder); LoadDest(np); }
}

static int DestUp(void) {
    int n, i, cut;
    if (s_destPath[0] == 0) return 0;
    n = SmLen(s_destPath);
    if (n > 0 && s_destPath[n - 1] == '\\') s_destPath[--n] = 0;
    cut = -1;
    for (i = n - 1; i >= 0; i--) { if (s_destPath[i] == '\\') { cut = i; break; } }
    if (cut <= 2) { LoadDest(""); return 1; }
    s_destPath[cut] = 0;
    { char tmp[SM_PATH]; SmCopy(tmp, sizeof(tmp), s_destPath); LoadDest(tmp); }
    return 1;
}

static void MoveCur(int* cursor, int* scroll, int count, int dir, int visible) {
    int c = *cursor + dir;
    if (c < 0) c = 0;
    if (c > count - 1) c = count - 1;
    if (c < 0) c = 0;
    *cursor = c;
    if (c < *scroll) *scroll = c;
    if (c >= *scroll + visible) *scroll = c - visible + 1;
}

static void SelGamePath(char* out, int cap) {
    SmJoin(out, cap, UDATA_ROOT, s_list[s_cursor].folder);
}

/* ---- public ------------------------------------------------------------ */

void SaveMgr_Enter(void) {
    s_state = SM_LIST;
    s_msg[0] = 0;
    Mu_MountAll();
    LoadGames();
}

int SaveMgr_Update(WORD pressed, WORD held) {
    const int VIS = 9;
    (void)held;

    if (s_state == SM_DEST) {
        if (pressed & BTN_DPAD_DOWN) { MoveCur(&s_destCursor, &s_destScroll, s_destCount, 1, VIS); Audio_PlaySfx(SFX_NAV_DOWN); }
        if (pressed & BTN_DPAD_UP) { MoveCur(&s_destCursor, &s_destScroll, s_destCount, -1, VIS); Audio_PlaySfx(SFX_NAV_UP); }
        if (pressed & BTN_A) { DestEnter(); Audio_PlaySfx(SFX_SELECT); }
        if (pressed & BTN_X) { if (DestUp()) Audio_PlaySfx(SFX_BACK); }
        if (pressed & BTN_WHITE) {
            if (s_destPath[0]) {
                char src[SM_PATH], dst[SM_PATH];
                int ok;
                SelGamePath(src, sizeof(src));
                SmJoin(dst, sizeof(dst), s_destPath, s_list[s_cursor].folder);
                ok = s_pendMove ? Fileops_Move(src, dst) : Fileops_CopyTree(src, dst);
                SmSetMsg(ok ? (s_pendMove ? "Moved" : "Copied") : "Operation failed");
                if (ok && s_pendMove) LoadGames();
                s_state = SM_LIST;
                Audio_PlaySfx(SFX_SELECT);
            }
            else SmSetMsg("Pick a folder first");
        }
        if (pressed & BTN_B) { s_state = SM_LIST; SmSetMsg("Cancelled"); Audio_PlaySfx(SFX_BACK); }
        return 0;
    }

    if (s_state == SM_CONFIRM_DEL) {
        if (pressed & BTN_A) {
            char src[SM_PATH];
            int ok;
            SelGamePath(src, sizeof(src));
            ok = Fileops_Delete(src);
            SmSetMsg(ok ? "Deleted" : "Delete failed");
            LoadGames();
            s_state = SM_LIST;
            Audio_PlaySfx(ok ? SFX_SELECT : SFX_BACK);
        }
        if (pressed & BTN_B) { s_state = SM_LIST; Audio_PlaySfx(SFX_BACK); }
        return 0;
    }

    if (s_state == SM_OPS) {
        if (pressed & BTN_DPAD_DOWN) { if (s_opCursor < 2) { s_opCursor++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_opCursor > 0) { s_opCursor--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & BTN_B) { s_state = SM_LIST; Audio_PlaySfx(SFX_BACK); return 0; }
        if (pressed & BTN_A) {
            if (s_opCursor <= 1) {
                s_pendMove = (s_opCursor == 1);
                LoadDest("");
                s_state = SM_DEST;
                SmSetMsg(s_pendMove ? "Move: pick dest, WHITE" : "Copy: pick dest, WHITE");
                Audio_PlaySfx(SFX_SELECT);
            }
            else {
                s_state = SM_CONFIRM_DEL;
                Audio_PlaySfx(SFX_SELECT);
            }
        }
        return 0;
    }

    /* ---- flat game list ---- */
    if (pressed & BTN_DPAD_DOWN) { MoveCur(&s_cursor, &s_scroll, s_count, 1, VIS); LoadArt(); Audio_PlaySfx(SFX_NAV_DOWN); }
    if (pressed & BTN_DPAD_UP) { MoveCur(&s_cursor, &s_scroll, s_count, -1, VIS); LoadArt(); Audio_PlaySfx(SFX_NAV_UP); }
    if (pressed & BTN_B) { FreeArt(); Audio_PlaySfx(SFX_BACK); return 1; }
    if ((pressed & BTN_A) && s_count > 0) {
        s_opCursor = 0;
        s_state = SM_OPS;
        Audio_PlaySfx(SFX_SELECT);
    }
    return 0;
}

/* ---- render ------------------------------------------------------------ */

/* simple themed line border -- the art panel (part of the layout, not a popup) */
static void DrawArtBorder(IDirect3DDevice8* d, float x, float y, float w, float h) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    int ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    (void)d;
    UI_FillRect(x, y, w, h, UI_ARGB(235, 6, 10, 8));
    UI_FillRect(x, y, w, 2.0f, UI_ARGB(255, ar, ag, ab));
    UI_FillRect(x, y + h - 2.0f, w, 2.0f, UI_ARGB(255, ar, ag, ab));
    UI_FillRect(x, y, 2.0f, h, UI_ARGB(255, ar, ag, ab));
    UI_FillRect(x + w - 2.0f, y, 2.0f, h, UI_ARGB(255, ar, ag, ab));
}

/* themed popup dialog -- dims the scene then draws the menu frame, exactly like
   FileManager's operation dialogs. */
static void DrawDialog(IDirect3DDevice8* d, float x, float y, float w, float h) {
    const Texture* frame = Theme_Asset("frame_menu_v");
    (void)d;
    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(150, 0, 0, 0));
    if (frame) UI_DrawSprite(frame, x, y, w, h, 0xFFFFFFFF, 0);
    else       UI_FillRect(x, y, w, h, UI_ARGB(235, 18, 22, 18));
}

void SaveMgr_Render(void) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* frame = Theme_Asset("frame_menu_v");
    const Texture* foot = Theme_Asset("bar_footer");
    const char* footHint = "A ACTIONS   B EXIT";
    float artX = 44.0f, artY = 96.0f, artW = 256.0f, artH = 256.0f;
    float listX = 320.0f, listY = 70.0f, listW = 296.0f, listH = 350.0f;
    const int VIS = 9;
    int i;

    Backdrop_Draw();

    Font_DrawText(d, 24.0f, 12.0f, "SAVE MANAGER", FONT_SIZE_MEDIUM, accent, 0);
    if (s_msg[0]) Font_DrawTextRight(d, 624.0f, 16.0f, s_msg, FONT_SIZE_SMALL, glow);

    /* --- art panel --- */
    DrawArtBorder(d, artX - 8.0f, artY - 8.0f, artW + 16.0f, artH + 16.0f);
    if (s_art.tex) {
        UI_DrawSprite(&s_art, artX, artY, artW, artH, 0xFFFFFFFF, 0);
    }
    else {
        Font_DrawText(d, artX + 60.0f, artY + artH * 0.5f - 8.0f, "No image", FONT_SIZE_SMALL, dim, 0);
    }
    if (s_count > 0)
        Font_DrawText(d, artX, artY + artH + 14.0f, s_list[s_cursor].name, FONT_SIZE_SMALL, text, (int)artW);

    /* --- game list --- */
    if (frame) UI_DrawSprite(frame, listX - 12.0f, listY - 10.0f, listW + 24.0f, listH + 28.0f, 0xFFFFFFFF, 0);
    Font_DrawText(d, listX + 12.0f, listY + 4.0f, "GAMES", FONT_SIZE_SMALL, accent, 0);
    if (s_count == 0) {
        Font_DrawText(d, listX + 12.0f, listY + 44.0f, "No saves found", FONT_SIZE_MEDIUM, dim, 0);
    }
    else {
        for (i = 0; i < VIS && (s_scroll + i) < s_count; i++) {
            int idx = s_scroll + i;
            float ry = listY + 40.0f + (float)i * 32.0f;
            if (idx == s_cursor)
                UI_FillRect(listX + 6.0f, ry - 2.0f, listW - 12.0f, 28.0f, UI_ARGB(95, ar, ag, ab));
            Font_DrawText(d, listX + 16.0f, ry, s_list[idx].name, FONT_SIZE_MEDIUM,
                (idx == s_cursor) ? glow : text, (int)(listW - 28.0f));
        }
    }

    /* --- overlays --- */
    if (s_state == SM_OPS) {
        float bx = 232.0f, by = 150.0f, bw = 176.0f, bh = 140.0f;
        DrawDialog(d, bx, by, bw, bh);
        Font_DrawText(d, bx + 20.0f, by + 16.0f, "ACTIONS", FONT_SIZE_SMALL, accent, 0);
        for (i = 0; i < 3; i++) {
            float ry = by + 44.0f + (float)i * 26.0f;
            if (i == s_opCursor)
                UI_FillRect(bx + 12.0f, ry - 2.0f, bw - 24.0f, 22.0f, UI_ARGB(90, ar, ag, ab));
            Font_DrawText(d, bx + 22.0f, ry, k_opNames[i], FONT_SIZE_SMALL,
                (i == s_opCursor) ? glow : text, 0);
        }
        footHint = "A SELECT   B CANCEL";
    }
    else if (s_state == SM_CONFIRM_DEL) {
        float bx = 200.0f, by = 190.0f, bw = 240.0f, bh = 120.0f;
        DrawDialog(d, bx, by, bw, bh);
        Font_DrawText(d, bx + 20.0f, by + 18.0f, "Delete all saves", FONT_SIZE_MEDIUM, accent, 0);
        Font_DrawText(d, bx + 20.0f, by + 48.0f, "for this game?", FONT_SIZE_SMALL, text, 0);
        if (s_count > 0)
            Font_DrawText(d, bx + 20.0f, by + 68.0f, s_list[s_cursor].name, FONT_SIZE_SMALL, glow, (int)(bw - 40.0f));
        Font_DrawText(d, bx + 20.0f, by + 92.0f, "A = yes    B = no", FONT_SIZE_SMALL, dim, 0);
        footHint = "A YES   B NO";
    }
    else if (s_state == SM_DEST) {
        float bx = 180.0f, by = 70.0f, bw = 320.0f, bh = 350.0f;
        char title[SM_PATH];
        DrawDialog(d, bx, by, bw, bh);
        SmCopy(title, sizeof(title), "DEST: ");
        SmCat(title, sizeof(title), s_destPath[0] ? s_destPath : "(pick a drive)");
        Font_DrawText(d, bx + 14.0f, by + 10.0f, title, FONT_SIZE_SMALL, accent, (int)(bw - 28.0f));
        if (s_destCount == 0) {
            Font_DrawText(d, bx + 14.0f, by + 44.0f, "(empty)", FONT_SIZE_SMALL, dim, 0);
        }
        else {
            for (i = 0; i < VIS && (s_destScroll + i) < s_destCount; i++) {
                int idx = s_destScroll + i;
                float ry = by + 44.0f + (float)i * 30.0f;
                if (idx == s_destCursor)
                    UI_FillRect(bx + 8.0f, ry - 2.0f, bw - 16.0f, 26.0f, UI_ARGB(95, ar, ag, ab));
                Font_DrawText(d, bx + 16.0f, ry, s_dest[idx].name, FONT_SIZE_MEDIUM,
                    (idx == s_destCursor) ? glow : text, (int)(bw - 30.0f));
            }
        }
        footHint = "A ENTER  X UP  WHITE PASTE  B CANCEL";
    }

    if (foot) UI_DrawSprite(foot, 8.0f, 442.0f, 624.0f, 32.0f, 0xFFFFFFFF, 0);
    Font_DrawText(d, 24.0f, 449.0f, footHint, FONT_SIZE_SMALL, text, 0);
}