/*---------------------------------------------------------------------------
    dd_launcher.cpp -- shared app-browser engine.

    A1: scans each configured root with FindFirstFile/FindNextFile, collects
    every immediate subfolder that contains a default.xbe, and presents them
    as a scrollable, cursor-navigable list in the menu frame. Folder name is
    the label for now (A2 replaces it with the real XBE certificate title).
    Read-only -- safe on xemu. Missing drives/roots are skipped silently.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <string.h>
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_iso.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "font.h"
#include "input.h"
#include "dd_audio.h"
#include "dd_xbe.h"
#include "dd_mount.h"
#include "dd_recents.h"
#include "dd_paths.h"
#include "dd_browse.h"
#include "dd_synopsis.h"
#include "dd_backdrop.h"
#include "dd_pedestal.h"
#include "dd_launcher.h"

#define LAUNCH_MAX_APPS     256
#define LAUNCH_NAME_MAX     64
#define LAUNCH_PATH_MAX     256
#define LAUNCH_ROWS_VISIBLE 9

typedef struct {
    char     label[LAUNCH_NAME_MAX];   /* display text (cert title, A2)      */
    char     xbePath[LAUNCH_PATH_MAX]; /* full path to its default.xbe       */
    unsigned titleId;                  /* cert title id (cache key / A3+)    */
} LaunchItem;

static LaunchItem            s_items[LAUNCH_MAX_APPS];
static int                   s_count = 0;
static int                   s_cursor = 0;
static int                   s_scroll = 0;
static const LauncherConfig* s_cfg = NULL;

/* pedestal title image: only the selected app's image is decoded (lazy) */
static Texture s_ped = { 0, 0, 0, 0, 0 };
static int     s_pedIdx = -1;
static int     s_pedFlat = 0;   /* 1 = art is _resources case art -> flat hologram */

static void JoinPath(char* out, int cap, const char* a, const char* b);  /* fwd */

/* ---- _resources support ------------------------------------------------
   A title folder may carry a "_resources" subfolder (the standard Xbox
   artwork/metadata pack). When present we prefer:
     - _resources\artwork\opencase.png  as the pedestal image (already 3D)
     - _resources\default.xml  <title>  as the display label
   Falls back to the XBE cert title image / folder name when absent. ------ */

   /* copy the title folder (xbePath with the trailing "\default.xbe" removed) */
static void TitleFolder(const char* xbePath, char* out, int cap) {
    int n = 0, last = -1, i;
    while (xbePath[n] && n < cap - 1) { out[n] = xbePath[n]; n++; }
    out[n] = 0;
    for (i = 0; i < n; i++) if (out[i] == '\\') last = i;
    if (last >= 0) out[last] = 0;     /* drop "\default.xbe" */
}

/* tiny XML scan: copy the text between <tag> and </tag> into out. Returns 1
   if found. No allocations; flat schema only. */
static int XmlTag(const char* xml, const char* tag, char* out, int cap) {
    char open[40], close[40];
    int oi = 0, ci = 0, i;
    const char* p; const char* s;
    open[oi++] = '<'; for (i = 0; tag[i] && oi < 38; i++) open[oi++] = tag[i]; open[oi++] = '>'; open[oi] = 0;
    close[ci++] = '<'; close[ci++] = '/'; for (i = 0; tag[i] && ci < 38; i++) close[ci++] = tag[i]; close[ci++] = '>'; close[ci] = 0;

    /* find <tag> */
    for (p = xml; *p; p++) {
        const char* a = p; const char* b = open; int m = 1;
        while (*b) { if (*a != *b) { m = 0; break; } a++; b++; }
        if (m) { s = a; goto found; }
    }
    return 0;
found:
    {   /* copy until </tag> or buffer full */
        int n = 0;
        const char* q = s;
        while (*q && n < cap - 1) {
            const char* a = q; const char* b = close; int m = 1;
            while (*b) { if (*a != *b) { m = 0; break; } a++; b++; }
            if (m) break;
            out[n++] = *q++;
        }
        out[n] = 0;
        /* trim leading/trailing CR/LF/space */
        while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = 0;
        return 1;
    }
}

/* read _resources\default.xml <title> for the title at xbePath. 1 if found. */
static int ResTitle(const char* xbePath, char* out, int cap) {
    char folder[LAUNCH_PATH_MAX], path[LAUNCH_PATH_MAX];
    char buf[1536];
    HANDLE h; DWORD got = 0; int ok;
    TitleFolder(xbePath, folder, sizeof(folder));
    JoinPath(path, sizeof(path), folder, "_resources\\default.xml");
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    if (got == 0) return 0;
    buf[got] = 0;
    ok = XmlTag(buf, "title", out, cap);
    return (ok && out[0]) ? 1 : 0;
}

/* try to load _resources\artwork\opencase.png into tex. 1 on success. */
static int ResOpencase(const char* xbePath, Texture* tex) {
    char folder[LAUNCH_PATH_MAX], path[LAUNCH_PATH_MAX];
    DWORD attr;
    TitleFolder(xbePath, folder, sizeof(folder));
    JoinPath(path, sizeof(path), folder, "_resources\\artwork\\opencase.png");
    attr = GetFileAttributesA(path);
    if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    return Texture_LoadPNG(path, tex) ? 1 : 0;
}

/* try _resources\artwork\poster.jpg (XBMC4Gamers packs ship JPG box art). 1 on
   success. Uses the picojpeg-backed loader. */
static int ResPoster(const char* xbePath, Texture* tex) {
    char folder[LAUNCH_PATH_MAX], path[LAUNCH_PATH_MAX];
    DWORD attr;
    TitleFolder(xbePath, folder, sizeof(folder));
    JoinPath(path, sizeof(path), folder, "_resources\\artwork\\poster.jpg");
    attr = GetFileAttributesA(path);
    if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    return Texture_LoadJPEG(path, tex) ? 1 : 0;
}

/* Public: load cover art for any title using the same priority chain the
   launcher uses. *isFlat -> 1 if the art is _resources case art (draw as a
   hologram), 0 if it's a title image / placeholder (draw on the cube). Returns
   1 if any texture was loaded. Used by the launcher and the screensaver. */
int Launcher_LoadArtFor(const char* xbePath, Texture* out, int* isFlat) {
    if (isFlat) *isFlat = 0;
    if (!out || !xbePath || !xbePath[0]) return 0;
    out->tex = NULL;
    if (ResOpencase(xbePath, out)) {
        if (isFlat) *isFlat = 1;
        return 1;
    }
    if (ResPoster(xbePath, out)) {       /* JPG box art from the pack */
        if (isFlat) *isFlat = 1;
        return 1;
    }
    if (Xbe_LoadTitleImage(Gfx_Device(), xbePath, out)) return 1;
    {   /* placeholder cover: active theme's, else default */
        char ph[260];
        Theme_ResolveIcon("s2_003.png", ph, sizeof(ph));
        if (Texture_LoadPNG(ph, out)) return 1;
    }
    return 0;
}

/* decode (or clear) the title image for item 'idx'; no-op if unchanged */
static void LoadPedestal(int idx) {
    if (idx == s_pedIdx) return;
    if (s_ped.tex) Texture_Release(&s_ped);
    s_ped.tex = NULL;
    s_pedIdx = idx;
    s_pedFlat = 0;
    if (idx < 0 || idx >= s_count) return;
    Launcher_LoadArtFor(s_items[idx].xbePath, &s_ped, &s_pedFlat);
}

/* join "a" + "\" + "b" into out (no sprintf) */
static void JoinPath(char* out, int cap, const char* a, const char* b) {
    int n;
    out[0] = 0;
    strncpy(out, a, cap - 1); out[cap - 1] = 0;
    n = (int)strlen(out);
    if (n > 0 && out[n - 1] != '\\' && n < cap - 1) { out[n] = '\\'; out[n + 1] = 0; }
    strncat(out, b, cap - strlen(out) - 1);
}

/* ---- scan-result cache ------------------------------------------------------
   The folder walk (FindFirstFile) is cheap, but reading + parsing each XBE's
   certificate (Xbe_ReadTitle: open, read 8KB, decode UTF-16 name) is the slow
   part when a drive holds many apps. We cache {xbePath -> label, titleId} to
   D:\data\cache\<id>.lc. On re-entry we still enumerate folders (so adds and
   removes are picked up immediately), but a folder already in the cache skips
   the cert parse entirely. New folders get parsed once; the cache is rewritten
   after. Guarded: a read-only volume (xemu) just never persists -- scanning
   still works, only the speedup is lost. ----------------------------------- */

#define CACHE_DIR   "D:\\data\\cache"
#define CACHE_MAGIC 0x314C4C44u   /* 'DLL1' */

static LaunchItem s_cache[LAUNCH_MAX_APPS];
static int        s_cacheCount = 0;

static void CachePath(const char* id, char* out, int cap) {
    out[0] = 0;
    strncpy(out, CACHE_DIR "\\", cap - 1); out[cap - 1] = 0;
    strncat(out, id, cap - strlen(out) - 1);
    strncat(out, ".lc", cap - strlen(out) - 1);
}

static void CacheLoad(const char* id) {
    char   path[LAUNCH_PATH_MAX];
    HANDLE h;
    DWORD  magic = 0, got = 0;
    int    n = 0;

    s_cacheCount = 0;
    if (!id) return;
    CachePath(id, path, sizeof(path));
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    if (ReadFile(h, &magic, sizeof(magic), &got, NULL) && got == sizeof(magic) &&
        magic == CACHE_MAGIC &&
        ReadFile(h, &n, sizeof(n), &got, NULL) && got == sizeof(n) &&
        n > 0 && n <= LAUNCH_MAX_APPS) {
        DWORD want = (DWORD)n * sizeof(LaunchItem);
        if (ReadFile(h, s_cache, want, &got, NULL) && got == want)
            s_cacheCount = n;
    }
    CloseHandle(h);
}

/* fill labelOut (LAUNCH_NAME_MAX) + *tidOut from the cache; 1 if found */
static int CacheFind(const char* xbePath, char* labelOut, unsigned* tidOut) {
    int i;
    for (i = 0; i < s_cacheCount; i++) {
        if (strcmp(s_cache[i].xbePath, xbePath) == 0) {
            strncpy(labelOut, s_cache[i].label, LAUNCH_NAME_MAX - 1);
            labelOut[LAUNCH_NAME_MAX - 1] = 0;
            *tidOut = s_cache[i].titleId;
            return 1;
        }
    }
    return 0;
}

static void CacheSave(const char* id) {
    char   path[LAUNCH_PATH_MAX];
    HANDLE h;
    DWORD  magic = CACHE_MAGIC, wrote = 0;

    if (!id || s_count <= 0) return;
    CreateDirectoryA("D:\\data", NULL);      /* parent first */
    CreateDirectoryA(CACHE_DIR, NULL);       /* then cache subdir; ignore exists */
    CachePath(id, path, sizeof(path));
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;   /* read-only volume -> skip quietly */

    WriteFile(h, &magic, sizeof(magic), &wrote, NULL);
    WriteFile(h, &s_count, sizeof(s_count), &wrote, NULL);
    WriteFile(h, s_items, (DWORD)s_count * sizeof(LaunchItem), &wrote, NULL);
    CloseHandle(h);
}

/* enumerate immediate subfolders of root that contain a default.xbe */
static void ScanRoot(const char* root) {
    char            pattern[LAUNCH_PATH_MAX];
    char            folder[LAUNCH_PATH_MAX];
    char            xbe[LAUNCH_PATH_MAX];
    WIN32_FIND_DATA fd;
    HANDLE          h;

    JoinPath(pattern, sizeof(pattern), root, "*");
    h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;   /* root/drive not present */

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;            /* "." and ".." */
        if (s_count >= LAUNCH_MAX_APPS) break;

        JoinPath(folder, sizeof(folder), root, fd.cFileName);
        JoinPath(xbe, sizeof(xbe), folder, "default.xbe");
        if (GetFileAttributes(xbe) != 0xFFFFFFFF) {       /* file exists */
            LaunchItem* it = &s_items[s_count];
            char nm[LAUNCH_NAME_MAX];
            unsigned tid = 0;

            strncpy(it->xbePath, xbe, LAUNCH_PATH_MAX - 1);
            it->xbePath[LAUNCH_PATH_MAX - 1] = 0;

            /* label priority: _resources\default.xml <title>, then the XBE
               cert title (cache or fresh read), then the folder name. */
            if (ResTitle(it->xbePath, nm, sizeof(nm)) && nm[0]) {
                strncpy(it->label, nm, LAUNCH_NAME_MAX - 1);
                it->label[LAUNCH_NAME_MAX - 1] = 0;
                /* still resolve titleId from cache/cert for the launch key */
                if (!CacheFind(it->xbePath, nm, &tid))
                    Xbe_ReadTitle(xbe, nm, sizeof(nm), &tid);
                it->titleId = tid;
            }
            else if (CacheFind(it->xbePath, it->label, &tid)) {
                it->titleId = tid;
            }
            else if (Xbe_ReadTitle(xbe, nm, sizeof(nm), &tid) && nm[0]) {
                strncpy(it->label, nm, LAUNCH_NAME_MAX - 1);
                it->label[LAUNCH_NAME_MAX - 1] = 0;
                it->titleId = tid;
            }
            else {
                strncpy(it->label, fd.cFileName, LAUNCH_NAME_MAX - 1);
                it->label[LAUNCH_NAME_MAX - 1] = 0;
                it->titleId = tid;
            }

            s_count++;
        }
    } while (FindNextFile(h, &fd));

    FindClose(h);
}

/* case-insensitive compare: <0, 0, >0 */
static int LabelCmp(const char* a, const char* b) {
    int i = 0;
    for (;;) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0)  return 0;
        i++;
    }
}

/* sort s_items alphabetically by label (insertion sort; n is small) */
static void SortItems(void) {
    int i, j;
    for (i = 1; i < s_count; i++) {
        LaunchItem key = s_items[i];
        j = i - 1;
        while (j >= 0 && LabelCmp(s_items[j].label, key.label) > 0) {
            s_items[j + 1] = s_items[j];
            j--;
        }
        s_items[j + 1] = key;
    }
}

static void ClampScroll(void) {
    if (s_cursor < s_scroll) s_scroll = s_cursor;
    if (s_cursor >= s_scroll + LAUNCH_ROWS_VISIBLE)
        s_scroll = s_cursor - LAUNCH_ROWS_VISIBLE + 1;
    if (s_scroll < 0) s_scroll = 0;
}

/* (re)scan the current category's built-in roots + user custom paths into the
   item list. Preserves nothing -- callers manage cursor/scroll around it. */
static void RescanCurrent(void) {
    int i;
    if (!s_cfg) return;
    s_count = 0;
    CacheLoad(s_cfg->cacheId);               /* prior cert results, if any */
    for (i = 0; i < s_cfg->rootCount; i++)
        ScanRoot(s_cfg->roots[i]);
    /* also scan any user-added custom paths for this category */
    {
        int np = Paths_Count(s_cfg->cacheId), k;
        for (k = 0; k < np; k++)
            ScanRoot(Paths_Get(s_cfg->cacheId, k));
    }
    CacheSave(s_cfg->cacheId);                /* refresh cache with this scan */
    SortItems();                              /* alphabetical by label */
}

void Launcher_Enter(const LauncherConfig* cfg) {
    s_cfg = cfg;
    s_count = 0; s_cursor = 0; s_scroll = 0;
    if (!cfg) return;
    RescanCurrent();
    s_pedIdx = -1;
    LoadPedestal(s_cursor);   /* decode the first app's title image */
}

int Launcher_Update(WORD pressed, WORD held) {
    (void)held;   /* hold-to-repeat scroll arrives in A5 */

    /* If the folder picker is open it owns all input. On confirm, add the
       chosen folder as a custom path for this category, persist, and re-scan
       so the new titles appear immediately (no exit/re-enter needed). */
    if (Browse_IsOpen()) {
        int r = Browse_Update(pressed);
        if (r == 1) {
            char picked[256];
            Browse_GetPath(picked, sizeof(picked));
            if (s_cfg && picked[0]) {
                if (Paths_Add(s_cfg->cacheId, picked)) {
                    Paths_Save();
                    s_cursor = 0; s_scroll = 0;
                    RescanCurrent();
                    s_pedIdx = -1;
                    LoadPedestal(s_cursor);
                }
            }
        }
        return 0;   /* swallow input while/after the overlay handled it */
    }

    /* Synopsis popup owns input while open. */
    if (Synopsis_IsOpen()) {
        Synopsis_Update(pressed);
        return 0;
    }

    if (pressed & BTN_B) {
        Audio_PlaySfx(SFX_BACK);
        if (s_ped.tex) Texture_Release(&s_ped);
        s_ped.tex = NULL; s_pedIdx = -1; s_pedFlat = 0;
        return 1;
    }

    /* WHITE = title info: open the synopsis popup for the highlighted title,
       but only if it actually has a _resources pack. Silent no-op otherwise. */
    if (pressed & BTN_WHITE) {
        if (s_count > 0 && s_cursor >= 0 && s_cursor < s_count) {
            const char* xp = s_items[s_cursor].xbePath;
            if (Synopsis_Available(xp)) {
                Audio_PlaySfx(SFX_SELECT);
                Synopsis_Open(xp);
                return 0;
            }
        }
    }

    /* X = refresh: re-scan the current category in place (handy after copying
       new titles over FTP -- no need to leave and re-enter the menu). */
    if (pressed & BTN_X) {
        int keep = s_cursor;
        Audio_PlaySfx(SFX_SELECT);
        RescanCurrent();
        s_cursor = (keep < s_count) ? keep : (s_count > 0 ? s_count - 1 : 0);
        if (s_cursor < 0) s_cursor = 0;
        ClampScroll();
        s_pedIdx = -1;
        LoadPedestal(s_cursor);
    }

    /* Y = add a scan path: open the folder picker. The category title gives
       context ("Add GAMES folder"). */
    if (pressed & BTN_Y) {
        char ttl[40];
        int i = 0; const char* t = s_cfg ? s_cfg->title : "folder";
        const char* pre = "Add ";
        while (pre[i]) { ttl[i] = pre[i]; i++; }
        { int j = 0; while (t[j] && i < 33) ttl[i++] = t[j++]; }
        { const char* suf = " path"; int j = 0; while (suf[j] && i < 39) ttl[i++] = suf[j++]; }
        ttl[i] = 0;
        Browse_Open(ttl);
        return 0;
    }

    if (s_count > 0) {
        if (pressed & BTN_DPAD_DOWN) {
            if (s_cursor < s_count - 1) { s_cursor++; Audio_PlaySfx(SFX_NAV_DOWN); ClampScroll(); LoadPedestal(s_cursor); }
        }
        if (pressed & BTN_DPAD_UP) {
            if (s_cursor > 0) { s_cursor--; Audio_PlaySfx(SFX_NAV_UP); ClampScroll(); LoadPedestal(s_cursor); }
        }
        /* LT / RT jump a full page -- big libraries are painful one row at a
           time. Page == the visible row count, so the cursor lands a screenful
           away and the list pages with it. Clamped to the list ends. */
        if (pressed & BTN_RTRIG) {
            if (s_cursor < s_count - 1) {
                s_cursor += LAUNCH_ROWS_VISIBLE;
                if (s_cursor > s_count - 1) s_cursor = s_count - 1;
                Audio_PlaySfx(SFX_NAV_DOWN); ClampScroll(); LoadPedestal(s_cursor);
            }
        }
        if (pressed & BTN_LTRIG) {
            if (s_cursor > 0) {
                s_cursor -= LAUNCH_ROWS_VISIBLE;
                if (s_cursor < 0) s_cursor = 0;
                Audio_PlaySfx(SFX_NAV_UP); ClampScroll(); LoadPedestal(s_cursor);
            }
        }
        if (pressed & BTN_A) {
            Audio_PlaySfx(SFX_SELECT);
            /* release the pedestal texture, stop the music, then hand off.
               Mount_LaunchXbe does not return on success. */
            if (s_ped.tex) Texture_Release(&s_ped);
            s_ped.tex = NULL; s_pedIdx = -1; s_pedFlat = 0;
            Audio_StopMusic();
            Recents_Add(s_items[s_cursor].label, s_items[s_cursor].xbePath);
            Mount_LaunchXbe(s_items[s_cursor].xbePath);
            /* fell through -> launch failed; carry on so the menu stays usable */
        }
    }
    return 0;
}

/* copy src into dst, truncating with ".." if longer than max visible chars */
static void FitLabel(char* dst, int cap, const char* src, int maxChars) {
    int n = (int)strlen(src);
    if (maxChars > cap - 1) maxChars = cap - 1;
    if (n <= maxChars) { strncpy(dst, src, cap - 1); dst[cap - 1] = 0; return; }
    strncpy(dst, src, maxChars - 2);
    dst[maxChars - 2] = '.'; dst[maxChars - 1] = '.'; dst[maxChars] = 0;
}

void Launcher_Render(void) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF);
    int   ag = (int)((accent >> 8) & 0xFF);
    int   ab = (int)(accent & 0xFF);

    const Texture* hdr = Theme_Asset("bar_header");
    const Texture* menu = Theme_Asset("frame_menu_v");
    const Texture* foot = Theme_Asset("bar_footer");
    const Texture* ped = Theme_Asset("platform_round");

    float menuX = 352.0f, menuY = 48.0f;
    float rowY0 = 86.0f, rowDY = 38.0f;
    const char* title = (s_cfg && s_cfg->title) ? s_cfg->title : "APPLICATIONS";
    int i, visible;

    /* shared ambient bloom (same green wash as the main menu) */
    Backdrop_Draw();

    /* header */
    if (hdr) UI_DrawSprite(hdr, 8.0f, 8.0f, 300.0f, 40.0f, 0xFFFFFFFF, 0);
    Font_DrawText(d, 26.0f, 14.0f, title, FONT_SIZE_LARGE, accent, 0);

    /* pedestal (left): platform base, then the light + title showpiece.
       _resources case art renders as a flat flickering hologram; an XBE title
       image (square) keeps the spinning cube. */
    if (ped) UI_DrawSprite(ped, 70.0f, 270.0f, 220.0f, 116.0f, 0xFFFFFFFF, 0);
    if (s_ped.tex && s_pedFlat) {
        Pedestal_DrawHologram(&s_ped, GetTickCount(), ar, ag, ab);
    }
    else {
        Pedestal_Draw(s_ped.tex ? &s_ped : NULL,
            Theme_Asset("overlay_selection_glow"),
            GetTickCount(), ar, ag, ab);
    }

    /* list frame (right), tilted to match the main menu */
    Iso_Begin();
    if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);

    if (s_count <= 0) {
        const char* em = (s_cfg && s_cfg->emptyMsg) ? s_cfg->emptyMsg : "Nothing found";
        Font_DrawTextIso(d, menuX + 30.0f, 206.0f, em, FONT_SIZE_SMALL, dim);
    }
    else {
        /* selection glow under the cursor row */
        float gy = rowY0 + rowDY * (float)(s_cursor - s_scroll) - 6.0f;
        Iso_FillRect(menuX + 6.0f, gy, 258.0f, 32.0f, UI_ARGB(80, ar, ag, ab), 1);

        visible = s_count - s_scroll;
        if (visible > LAUNCH_ROWS_VISIBLE) visible = LAUNCH_ROWS_VISIBLE;
        for (i = 0; i < visible; i++) {
            int  idx = s_scroll + i;
            char row[30];
            DWORD c = (idx == s_cursor) ? glow : text;
            FitLabel(row, sizeof(row), s_items[idx].label, 26);
            Font_DrawTextIso(d, menuX + 24.0f, rowY0 + rowDY * (float)i,
                row, FONT_SIZE_SMALL, c);
        }
    }

    Iso_End();

    /* footer */
    if (foot) UI_DrawSprite(foot, 8.0f, 442.0f, 624.0f, 32.0f, 0xFFFFFFFF, 0);
    Font_DrawText(d, 24.0f, 449.0f, "A LAUNCH  LT/RT PAGE  WHITE INFO  Y ADD PATH  X REFRESH  B BACK", FONT_SIZE_SMALL, text, 0);

    /* overlays on top of everything */
    Browse_Draw(d);
    Synopsis_Draw(d);
}