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
#include <stdlib.h>
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_iso.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "font.h"
#include "dd_hints.h"
#include "input.h"
#include "dd_watchdog.h"
#include "dd_audio.h"
#include "dd_xbe.h"
#include "dd_stbi.h"
#include "dd_mount.h"
#include "dd_storage.h"
#include "dd_smbsvc.h"
#include "dd_xview.h"
#include "dd_lcd.h"
#include "dd_recents.h"
#include "dd_paths.h"
#include "dd_browse.h"
#include "dd_synopsis.h"
#include "dd_isoui.h"
#include "dd_backdrop.h"
#include "dd_pedestal.h"
#include "dd_select.h"
#include "dd_dc.h"
#include "dd_typedart.h"
#include "dd_hidden.h"
#include "dd_launcher.h"
#define LAUNCH_MAX_APPS     2048
#define LAUNCH_MAX_DEPTH    4      /* game folders up to N levels below a root;
                                      1 = old flat behavior (immediate subfolders
                                      only). Only the recursion is capped -- a
                                      default.xbe is added wherever it's found. */
#define LAUNCH_NAME_MAX     64
#define LAUNCH_PATH_MAX     256
#define LAUNCH_ROWS_VISIBLE 9
#define LAUNCH_STATE_MAX    24

typedef struct {
    char     label[LAUNCH_NAME_MAX];   /* display text (cert title, A2)      */
    char     xbePath[LAUNCH_PATH_MAX]; /* full path to its default.xbe       */
    unsigned titleId;                  /* cert title id (cache key / A3+)    */
} LaunchItem;

static LaunchItem            s_items[LAUNCH_MAX_APPS];
static int                   s_count = 0;
static int                   s_cursor = 0;
static int                   s_scroll = 0;
static int                   s_showHidden = 0;   /* 1 = include hidden entries
                                                    in the list (marked) so they
                                                    can be un-hidden; reset on
                                                    every category enter */

                                                    /* ---- left-stick hold-to-scroll (with acceleration) ----------------------- */
#define ANALOG_SCROLL_TH   13000   /* |left-stick Y| to engage (deadzone is lower)   */
#define ANALOG_STEP_SLOW     240   /* ms between steps at the start of a hold        */
#define ANALOG_STEP_FAST      45   /* ms between steps once fully ramped up          */
#define ANALOG_RAMP_MS      1400   /* hold this long to reach full speed             */
#define ANALOG_PED_SETTLE    140   /* ms of no movement before cover art reloads     */
#define ANALOG_SFX_MS        110   /* min ms between nav tick sounds while scrolling  */

static int                   s_stickDir = 0;       /* -1 up, +1 down, 0 idle          */
static DWORD                 s_stickHoldStart = 0; /* tick when this hold engaged      */
static DWORD                 s_stickNextStep = 0;  /* next allowed step time           */
static DWORD                 s_stickNextSfx = 0;   /* next allowed nav-tick time       */
static int                   s_pedPending = 0;     /* cursor moved; art reload deferred */
static DWORD                 s_pedSettleAt = 0;    /* reload the pedestal once past this */
/* 1 while a launcher list is the active screen. Type-D reads the highlighted
   title via Launcher_CurrentAppName(); this flag makes that report NULL once the
   user backs out to the main menu so Type-D shows "DarkDash" instead of the
   stale last-highlighted title (the item arrays aren't cleared on leave). */
static int                   s_active = 0;
static const LauncherConfig* s_cfg = NULL;

/* pedestal title image: only the selected app's image is decoded (lazy) */
static Texture s_ped = { 0, 0, 0, 0, 0 };
static int     s_pedIdx = -1;
static int     s_pedFlat = 0;   /* 1 = art is _resources case art -> flat hologram */

/* Game-specific background art uses the same resource-pack folder as cover
   images. Only current and outgoing textures are retained during a fade. */
#define LAUNCH_FANART_FADE_MS  320
static Texture s_fanart = { 0, 0, 0, 0, 0 };
static Texture s_fanartOld = { 0, 0, 0, 0, 0 };
/* One warm texture for returning to a recently selected game. Ownership of
   each texture is exclusive to current, outgoing, or cached. */
static Texture s_fanartCached = { 0, 0, 0, 0, 0 };
static int s_fanartOldIdx = -1;
static int s_fanartCachedIdx = -1;
static DWORD s_fanartFadeAt = 0;
static int s_fanartCurrentIdx = -1;


/* Session-only navigation memory. Key by cacheId (stable for built-in and
   custom categories) so leaving a library and coming back lands on the same
   title and roughly the same row on screen. Nothing is written to disk. */
typedef struct {
    char cacheId[32];
    char xbePath[LAUNCH_PATH_MAX];
    int  rowOffset;                  /* cursor - scroll, 0..visible-1 */
    int  used;
} LaunchState;

static LaunchState s_state[LAUNCH_STATE_MAX];

static void JoinPath(char* out, int cap, const char* a, const char* b);  /* fwd */

static void QueuePedestalLoad(DWORD settleMs) {
    s_pedPending = 1;
    s_pedSettleAt = GetTickCount() + settleMs;
}

static LaunchState* FindState(const char* cacheId, int create) {
    int i, freeIdx = -1;
    if (!cacheId || !cacheId[0]) return NULL;
    for (i = 0; i < LAUNCH_STATE_MAX; i++) {
        if (s_state[i].used) {
            if (lstrcmpiA(s_state[i].cacheId, cacheId) == 0) return &s_state[i];
        }
        else if (freeIdx < 0) freeIdx = i;
    }
    if (!create || freeIdx < 0) return NULL;
    ZeroMemory(&s_state[freeIdx], sizeof(s_state[freeIdx]));
    lstrcpynA(s_state[freeIdx].cacheId, cacheId, (int)sizeof(s_state[freeIdx].cacheId));
    s_state[freeIdx].used = 1;
    return &s_state[freeIdx];
}

static void SaveNavState(void) {
    LaunchState* st;
    int off;
    if (!s_cfg || !s_cfg->cacheId || s_count <= 0 || s_cursor < 0 || s_cursor >= s_count) return;
    st = FindState(s_cfg->cacheId, 1);
    if (!st) return;
    lstrcpynA(st->xbePath, s_items[s_cursor].xbePath, (int)sizeof(st->xbePath));
    off = s_cursor - s_scroll;
    if (off < 0) off = 0;
    if (off >= LAUNCH_ROWS_VISIBLE) off = LAUNCH_ROWS_VISIBLE - 1;
    st->rowOffset = off;
}

static void RestoreNavState(void) {
    LaunchState* st;
    int i;
    if (!s_cfg || !s_cfg->cacheId || s_count <= 0) return;
    st = FindState(s_cfg->cacheId, 0);
    if (!st || !st->xbePath[0]) return;
    for (i = 0; i < s_count; i++) {
        if (lstrcmpiA(s_items[i].xbePath, st->xbePath) == 0) {
            s_cursor = i;
            s_scroll = s_cursor - st->rowOffset;
            if (s_scroll < 0) s_scroll = 0;
            if (s_scroll > s_count - LAUNCH_ROWS_VISIBLE && s_count > LAUNCH_ROWS_VISIBLE)
                s_scroll = s_count - LAUNCH_ROWS_VISIBLE;
            return;
        }
    }
}

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

/* The highlighted title's display name (resource-pack -> XBE cert -> folder,
   already resolved at scan time into .label). NULL if nothing is listed. */
const char* Launcher_CurrentAppName(void) {
    if (!s_active) return NULL;
    if (s_count <= 0) return NULL;
    if (s_cursor < 0 || s_cursor >= s_count) return NULL;
    return s_items[s_cursor].label;
}

/* Load a title's cover art straight to RGBA for off-screen use (the Type-D
   push). Mirrors the launcher's resource-pack priority -- opencase.png, then
   poster.jpg -- but returns raw pixels rather than a GPU texture. Returns a
   malloc'd RGBA buffer (free with DD_StbFree) and fills *w,*h, or NULL when the
   title has no resource-pack art. The embedded XBE title image is deliberately
   NOT covered here: it is stored GPU-swizzled (and sometimes DXT1), so the
   pack-less fallback is handled on its own path. */
static unsigned char* LoadRGBAFile(const char* path, int* w, int* h) {
    HANDLE         hf;
    DWORD          sz, got = 0, attr;
    unsigned char* file;
    unsigned char* rgba;

    attr = GetFileAttributesA(path);
    if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return NULL;
    sz = GetFileSize(hf, NULL);
    if (sz == 0 || sz == 0xFFFFFFFF) { CloseHandle(hf); return NULL; }
    file = (unsigned char*)malloc(sz);
    if (!file) { CloseHandle(hf); return NULL; }
    if (!ReadFile(hf, file, sz, &got, NULL) || got != sz) {
        free(file); CloseHandle(hf); return NULL;
    }
    CloseHandle(hf);
    rgba = DD_StbLoadImageMem(file, (int)sz, w, h);
    free(file);
    return rgba;
}

unsigned char* Launcher_LoadArtRGBA(const char* xbePath, int* w, int* h) {
    char folder[LAUNCH_PATH_MAX], path[LAUNCH_PATH_MAX];
    unsigned char* rgba;

    if (w) *w = 0;
    if (h) *h = 0;
    if (!xbePath || !xbePath[0]) return NULL;

    TitleFolder(xbePath, folder, sizeof(folder));

    JoinPath(path, sizeof(path), folder, "_resources\\artwork\\opencase.png");
    rgba = LoadRGBAFile(path, w, h);
    if (rgba) return rgba;

    JoinPath(path, sizeof(path), folder, "_resources\\artwork\\poster.jpg");
    rgba = LoadRGBAFile(path, w, h);
    return rgba;
}

/* The highlighted title's .xbe path. Unlike Launcher_CurrentAppName this is NOT
   gated on the launcher being the active screen, so Settings can ask "what is
   selected?" to push its art. NULL if nothing is listed. */
const char* Launcher_CurrentXbePath(void) {
    if (s_count <= 0) return NULL;
    if (s_cursor < 0 || s_cursor >= s_count) return NULL;
    return s_items[s_cursor].xbePath;
}

/* Free both fanart textures before leaving or rebuilding the launcher. */
static void ReleaseFanart(void) {
    if (s_fanart.tex) Texture_Release(&s_fanart);
    if (s_fanartOld.tex) Texture_Release(&s_fanartOld);
    if (s_fanartCached.tex) Texture_Release(&s_fanartCached);
    s_fanart.tex = NULL;
    s_fanartOld.tex = NULL;
    s_fanartCached.tex = NULL;
    s_fanartCurrentIdx = -1;
    s_fanartOldIdx = -1;
    s_fanartCachedIdx = -1;
    s_fanartFadeAt = 0;
}

/* Keep just one outgoing background as a warm decode cache. This is only
   called for an outgoing texture, never for the active texture. */
static void CacheFanart(Texture* art, int idx) {
    if (s_fanartCached.tex) Texture_Release(&s_fanartCached);
    s_fanartCached.tex = NULL;
    s_fanartCachedIdx = -1;
    if (art->tex && idx >= 0) {
        s_fanartCached = *art;
        art->tex = NULL;
        s_fanartCachedIdx = idx;
    }
    else if (art->tex) {
        Texture_Release(art);
        art->tex = NULL;
    }
}

static int LoadFanart(const char* xbePath, Texture* out) {
    char folder[LAUNCH_PATH_MAX], path[LAUNCH_PATH_MAX];
    static const char* const candidates[] = {
        "_resources\\artwork\\fanart-blur.jpg",
        "_resources\\artwork\\fanart.jpg",
        NULL
    };
    int i;
    if (!xbePath || !out) return 0;
    TitleFolder(xbePath, folder, sizeof(folder));
    for (i = 0; candidates[i]; ++i) {
        DWORD attrs;
        JoinPath(path, sizeof(path), folder, candidates[i]);
        attrs = GetFileAttributesA(path);
        if (attrs == 0xFFFFFFFF || (attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (Texture_LoadJPEG(path, out)) return 1;
    }
    return 0;
}

/* Called on the existing deferred pedestal-load path: no I/O during draw. */
static void LoadFanartFor(int idx) {
    Texture next = { 0, 0, 0, 0, 0 };
    DWORD now = GetTickCount();
    int previous = s_fanartCurrentIdx;
    if (idx == previous) return;

    /* Reuse either the fading-out texture or our one-entry warm cache.
       Decode only on a true cache miss; navigation is already debounced. */
    if (idx >= 0 && idx == s_fanartOldIdx && s_fanartOld.tex) {
        next = s_fanartOld;
        s_fanartOld.tex = NULL;
        s_fanartOldIdx = -1;
    }
    else if (idx >= 0 && idx == s_fanartCachedIdx && s_fanartCached.tex) {
        next = s_fanartCached;
        s_fanartCached.tex = NULL;
        s_fanartCachedIdx = -1;
    }
    else if (idx >= 0 && idx < s_count) {
        LoadFanart(s_items[idx].xbePath, &next);
    }
    if (s_fanartOld.tex) CacheFanart(&s_fanartOld, s_fanartOldIdx);
    s_fanartOldIdx = -1;
    if (s_fanart.tex) {
        s_fanartOld = s_fanart;
        s_fanart.tex = NULL;
        s_fanartOldIdx = previous;
    }
    s_fanart = next;
    s_fanartCurrentIdx = idx;
    s_fanartFadeAt = now;
}

/* Aspect-preserving center-fill: oversize rather than stretch the artwork. */
static void DrawFanart(const Texture* t, int alpha) {
    float iw, ih, scale, w, h;
    if (!t || !t->tex || alpha <= 0 || !t->w || !t->h) return;
    iw = (float)t->w; ih = (float)t->h;
    scale = 640.0f / iw;
    if (480.0f / ih > scale) scale = 480.0f / ih;
    w = iw * scale; h = ih * scale;
    UI_DrawSprite(t, (640.0f - w) * 0.5f, (480.0f - h) * 0.5f,
        w, h, UI_ARGB(alpha, 255, 255, 255), 0);
}

static void DrawFanartBackdrop(void) {
    DWORD now = GetTickCount();
    DWORD elapsed = now - s_fanartFadeAt;
    int incoming = 168;
    if (elapsed < LAUNCH_FANART_FADE_MS) {
        incoming = (int)(168u * elapsed / LAUNCH_FANART_FADE_MS);
        DrawFanart(&s_fanartOld, 168 - incoming);
    }
    else if (s_fanartOld.tex) {
        CacheFanart(&s_fanartOld, s_fanartOldIdx);
        s_fanartOldIdx = -1;
    }
    DrawFanart(&s_fanart, incoming);
    if (s_fanart.tex || (s_fanartOld.tex && elapsed < LAUNCH_FANART_FADE_MS)) {
        /* Theme colors remain visible and live text remains legible. */
        UI_FillGradient(0.0f, 0.0f, 640.0f, 480.0f,
            UI_ARGB(122, 2, 3, 9), UI_ARGB(176, 2, 3, 9),
            UI_ARGB(146, 2, 3, 9), UI_ARGB(200, 2, 3, 9), 0);
    }
}

/* decode (or clear) the title image for item 'idx'; no-op if unchanged */
static void LoadPedestal(int idx) {
    if (idx == s_pedIdx) return;
    if (s_ped.tex) Texture_Release(&s_ped);
    s_ped.tex = NULL;
    s_pedIdx = idx;
    s_pedFlat = 0;
    if (idx < 0 || idx >= s_count) { LoadFanartFor(-1); return; }
    Launcher_LoadArtFor(s_items[idx].xbePath, &s_ped, &s_pedFlat);
    if (s_ped.tex) Pedestal_Pulse();
    LoadFanartFor(idx);
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

/* enumerate subfolders of root that contain a default.xbe; descend into
   subfolders that don't (alphabetical buckets, category trees, etc.) up to
   LAUNCH_MAX_DEPTH levels. call with depth 0. */
static void ScanRoot(const char* root, int depth) {
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

            /* hide list: in normal browsing a hidden path is excluded entirely
               (the slot is reused). In show-hidden mode we keep it so the user
               can un-hide it; the render marks it. */
            if (!Hidden_Is(it->xbePath) || s_showHidden)
                s_count++;
        }
        else if (depth + 1 < LAUNCH_MAX_DEPTH) {
            ScanRoot(folder, depth + 1);   /* no default.xbe here -> descend */
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

/* uppercased first character of an item's label -- the "letter" it groups under.
   Digits/symbols just group under their own char, which is fine for jumping. */
static char ItemInitial(int idx) {
    char c;
    if (idx < 0 || idx >= s_count || !s_items[idx].label) return 0;
    c = s_items[idx].label[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return c;
}

/* Alpha-jump: hop the cursor a whole letter at a time through the (already
   alphabetical) list. dir +1 = start of the next letter group; dir -1 = start of
   the current group, or the previous group's start if already at the top of one.
   Big libraries get traversed in a few presses. */
static void AlphaJump(int dir) {
    int i = s_cursor;
    char cur;
    if (s_count <= 0) return;
    cur = ItemInitial(s_cursor);

    if (dir > 0) {
        while (i < s_count - 1 && ItemInitial(i) == cur) i++;   /* -> first of next letter */
    }
    else {
        while (i > 0 && ItemInitial(i - 1) == cur) i--;         /* -> start of this group  */
        if (i == s_cursor && i > 0) {                            /* already at top: prev group */
            char prev = ItemInitial(i - 1);
            i--;
            while (i > 0 && ItemInitial(i - 1) == prev) i--;
        }
    }

    if (i != s_cursor) {
        s_cursor = i;
        ClampScroll();
        QueuePedestalLoad(110);   /* skip decode if the user keeps jumping */
        Audio_PlaySfx(dir > 0 ? SFX_NAV_DOWN : SFX_NAV_UP);
    }
}

/* (re)scan the current category's built-in roots + user custom paths into the
   item list. Preserves nothing -- callers manage cursor/scroll around it. */
static void RescanCurrent(void) {
    int i;
    if (!s_cfg) return;
    DD_Storage_RefreshSecondary();         /* refresh N:/O:/P:/R: before scans */
    ReleaseFanart();  /* item indices change on scan; invalidate artwork identity */
    s_count = 0;
    CacheLoad(s_cfg->cacheId);               /* prior cert results, if any */
    for (i = 0; i < s_cfg->rootCount; i++)
        ScanRoot(s_cfg->roots[i], 0);
    /* Built-in equivalents on the CerBIOS secondary disk. These are
       optional additions; user-defined paths and cache IDs remain intact. */
    {
        int vi;
        for (vi = 0; vi < DD_Storage_Count(); vi++) {
            const DD_StorageVolume* v = DD_Storage_At(vi);
            char path[64];
            static const char* const appDirs[] = { "Apps", "Applications", NULL };
            static const char* const emuDirs[] = { "emu", "emulators", NULL };
            static const char* const gameDirs[] = { "Games", NULL };
            static const char* const hbDirs[] = { "Homebrew", NULL };
            const char* const* dirs = NULL;
            int j;
            if (!v || !v->disk || !DD_Storage_Ready(v)) continue;
            if (v->partition != 1 && v->partition != 6 && v->partition != 7) continue;
            if (!lstrcmpiA(s_cfg->cacheId, "apps")) dirs = appDirs;
            else if (!lstrcmpiA(s_cfg->cacheId, "emu")) dirs = emuDirs;
            else if (!lstrcmpiA(s_cfg->cacheId, "games")) dirs = gameDirs;
            else if (!lstrcmpiA(s_cfg->cacheId, "homebrew")) dirs = hbDirs;
            if (!dirs) continue;
            for (j = 0; dirs[j]; j++) {
                path[0] = v->letter; path[1] = ':'; path[2] = '\\'; path[3] = 0;
                lstrcatA(path, dirs[j]);
                ScanRoot(path, 0);
            }
        }
    }
    /* also scan any user-added custom paths for this category */
    {
        int np = Paths_Count(s_cfg->cacheId), k;
        for (k = 0; k < np; k++)
            ScanRoot(Paths_Get(s_cfg->cacheId, k), 0);
    }
    CacheSave(s_cfg->cacheId);                /* refresh cache with this scan */
    SortItems();                              /* alphabetical by label */
}

void Launcher_Enter(const LauncherConfig* cfg) {
    s_cfg = cfg;
    s_count = 0; s_cursor = 0; s_scroll = 0;
    s_showHidden = 0;        /* always enter in normal view */
    s_active = 1;             /* a launcher list is now the active screen */
    Select_Reset();           /* first launcher frame snaps to the restored row */
    if (!cfg) return;
    RescanCurrent();
    RestoreNavState();
    ReleaseFanart();
    s_pedIdx = -1;
    s_pedPending = 0;
    LoadPedestal(s_cursor);   /* decode the restored/first app's title image */
}

/* One-shot "NOW LOADING" screen for a launch: the selected title's pedestal +
   art centred like the screensaver, with NOW LOADING beneath. Rendered and
   presented ONCE; because the hand-off (Mount_LaunchXbe) does not return, this
   frame stays on the front buffer through the blocking Type-D art push and the
   launch, so the multi-second transfer reads as a deliberate load rather than a
   freeze. Uses s_ped, already decoded for the highlighted item. */
static void DrawLoadingScreen(void) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD          now = GetTickCount();
    const float    dxV = 140.0f, dyV = 20.0f;   /* screensaver recentre offsets */
    const Texture* ped;
    int            lw;

    if (!d) return;

    Gfx_BeginFrame(Theme_BG());

    /* themed platform base, same sprite/position the launcher and saver use */
    ped = Theme_Asset("platform_round");
    if (ped)
        UI_DrawSprite(ped, 70.0f + dxV, 270.0f + dyV, 220.0f, 116.0f,
            UI_ARGB(255, 255, 255, 255), 0);

    /* the title showpiece, centred, full opaque, fixed green beam (no drift) */
    if (s_ped.tex)
        Pedestal_DrawSaver(&s_ped, s_pedFlat, now, dxV, dyV, 255, 32, 255, 96);

    /* NOW LOADING centred under the platform (hero column 180 + recentre 140) */
    lw = Font_MeasureText("NOW LOADING", FONT_SIZE_MEDIUM);
    Font_DrawText(d, 320.0f - (float)lw * 0.5f, 415.0f, "NOW LOADING",
        FONT_SIZE_MEDIUM, UI_ARGB(255, 255, 255, 255), 0);

    Gfx_EndFrame();
}

int Launcher_Update(WORD pressed, WORD held) {
    (void)held;   /* analog hold-to-scroll (below) reads the stick directly via GetSticks */

    /* The ISO install wizard owns input while open. It drives dd_browse for
       its folder step, so it MUST be checked before the add-path Browse
       handler below (otherwise the folder confirm is read as an add-path). */
    if (IsoUi_IsOpen()) {
        int r = IsoUi_Update(pressed);
        if (r == 1) {                /* a game was installed -> show it now */
            RescanCurrent();
            s_pedIdx = -1;
            s_pedPending = 0;
            LoadPedestal(s_cursor);
        }
        return 0;
    }

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
                    s_pedPending = 0;
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
        SaveNavState();
        if (s_ped.tex) Texture_Release(&s_ped);
        s_ped.tex = NULL; s_pedIdx = -1; s_pedFlat = 0;
        ReleaseFanart();
        s_active = 0;        /* back to main menu: stop reporting a title to Type-D */
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
        SaveNavState();                 /* prefer the same title after sorting changes */
        RescanCurrent();
        s_cursor = (keep < s_count) ? keep : (s_count > 0 ? s_count - 1 : 0);
        if (s_cursor < 0) s_cursor = 0;
        ClampScroll();                  /* sensible index fallback if the title vanished */
        RestoreNavState();              /* otherwise restore by stable XBE path */
        ClampScroll();
        s_pedIdx = -1;
        s_pedPending = 0;
        LoadPedestal(s_cursor);
    }

    /* RIGHT-STICK CLICK = install an ISO to the HDD. GAMES category only.
       (Remap target: change BTN_RTHUMB below to taste.) */
    if ((pressed & BTN_RTHUMB) && s_cfg && s_cfg->cacheId &&
        s_cfg->cacheId[0] == 'g' && s_cfg->cacheId[1] == 'a' && s_cfg->cacheId[2] == 'm' &&
        s_cfg->cacheId[3] == 'e' && s_cfg->cacheId[4] == 's' && s_cfg->cacheId[5] == 0) {
        Audio_PlaySfx(SFX_SELECT);
        IsoUi_Open();
        return 0;
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

    /* BLACK = hide / un-hide the highlighted entry. In normal view this drops
       it from the list immediately (reversible). In show-hidden view it flips
       the persistent flag but keeps the row on screen so you can toggle freely;
       the marker reflects the current state. */
    if (pressed & BTN_BLACK) {
        if (s_count > 0 && s_cursor >= 0 && s_cursor < s_count) {
            const char* xp = s_items[s_cursor].xbePath;
            int wantHide = !Hidden_Is(xp);
            Hidden_Set(xp, wantHide);
            Audio_PlaySfx(SFX_SELECT);
            if (!s_showHidden && wantHide) {
                /* remove the just-hidden row in place; no full re-scan needed */
                int k;
                for (k = s_cursor; k < s_count - 1; k++) s_items[k] = s_items[k + 1];
                s_count--;
                if (s_cursor >= s_count) s_cursor = (s_count > 0) ? s_count - 1 : 0;
                ClampScroll();
                s_pedIdx = -1;
                s_pedPending = 0;
                LoadPedestal(s_cursor);
            }
        }
        return 0;
    }

    /* START = toggle the show-hidden view (so hidden entries can be reverted).
       Rebuilds the list with the filter on/off; keeps the cursor near where it
       was. */
    if (pressed & BTN_START) {
        int keep = s_cursor;
        s_showHidden = !s_showHidden;
        Audio_PlaySfx(SFX_SELECT);
        RescanCurrent();
        s_cursor = (keep < s_count) ? keep : (s_count > 0 ? s_count - 1 : 0);
        if (s_cursor < 0) s_cursor = 0;
        ClampScroll();
        s_pedIdx = -1;
        s_pedPending = 0;
        LoadPedestal(s_cursor);
        return 0;
    }

    if (s_count > 0) {
        if (pressed & BTN_DPAD_DOWN) {
            if (s_cursor < s_count - 1) { s_cursor++; Audio_PlaySfx(SFX_NAV_DOWN); ClampScroll(); QueuePedestalLoad(110); }
        }
        if (pressed & BTN_DPAD_UP) {
            if (s_cursor > 0) { s_cursor--; Audio_PlaySfx(SFX_NAV_UP); ClampScroll(); QueuePedestalLoad(110); }
        }
        /* D-Pad LEFT / RIGHT: alpha-jump -- skip to the previous / next letter
           group. Pairs with single-step (D-pad U/D), page (LT/RT), and analog
           hold-scroll for fast traversal of large libraries. */
        if (pressed & BTN_DPAD_RIGHT) AlphaJump(+1);
        if (pressed & BTN_DPAD_LEFT)  AlphaJump(-1);
        /* LT / RT jump a full page -- big libraries are painful one row at a
           time. Page == the visible row count, so the cursor lands a screenful
           away and the list pages with it. Clamped to the list ends. */
        if (pressed & BTN_RTRIG) {
            if (s_cursor < s_count - 1) {
                s_cursor += LAUNCH_ROWS_VISIBLE;
                if (s_cursor > s_count - 1) s_cursor = s_count - 1;
                Audio_PlaySfx(SFX_NAV_DOWN); ClampScroll(); QueuePedestalLoad(110);
            }
        }
        if (pressed & BTN_LTRIG) {
            if (s_cursor > 0) {
                s_cursor -= LAUNCH_ROWS_VISIBLE;
                if (s_cursor < 0) s_cursor = 0;
                Audio_PlaySfx(SFX_NAV_UP); ClampScroll(); QueuePedestalLoad(110);
            }
        }

        /* Left analog stick: hold-to-scroll, accelerating the longer it's held.
           Push up = previous, down = next. The cover-art (pedestal) decode is
           deferred until the scroll settles, and the nav tick is throttled, so
           fast scrolling stays smooth and doesn't machine-gun the SFX. */
        {
            int lx, ly, rx, ry, dir = 0;
            GetSticks(lx, ly, rx, ry);
            if (ly > ANALOG_SCROLL_TH)       dir = -1;   /* up   -> previous */
            else if (ly < -ANALOG_SCROLL_TH) dir = +1;   /* down -> next     */

            if (dir == 0) {
                s_stickDir = 0;                          /* released; settle loads art */
            }
            else {
                DWORD now = GetTickCount();
                if (dir != s_stickDir) {                 /* fresh push: step now, start ramp */
                    s_stickDir = dir;
                    s_stickHoldStart = now;
                    s_stickNextStep = now;
                    s_stickNextSfx = 0;
                }
                if (now >= s_stickNextStep) {
                    int moved = 0;
                    if (dir > 0) { if (s_cursor < s_count - 1) { s_cursor++; moved = 1; } }
                    else { if (s_cursor > 0) { s_cursor--; moved = 1; } }

                    if (moved) {
                        DWORD heldMs = now - s_stickHoldStart;
                        DWORD ramp = (heldMs > ANALOG_RAMP_MS) ? ANALOG_RAMP_MS : heldMs;
                        DWORD interval = ANALOG_STEP_SLOW -
                            (ramp * (ANALOG_STEP_SLOW - ANALOG_STEP_FAST) / ANALOG_RAMP_MS);
                        ClampScroll();
                        QueuePedestalLoad(ANALOG_PED_SETTLE); /* defer the art decode */
                        if (now >= s_stickNextSfx) {     /* throttle the tick */
                            Audio_PlaySfx(dir > 0 ? SFX_NAV_DOWN : SFX_NAV_UP);
                            s_stickNextSfx = now + ANALOG_SFX_MS;
                        }
                        s_stickNextStep = now + interval;
                    }
                    else {
                        s_stickNextStep = now + ANALOG_STEP_SLOW;  /* at an end: idle quietly */
                    }
                }
            }
        }

        /* Deferred cover-art load: once navigation has been quiet for the short
           settle window, decode the highlighted title's art once. D-pad, page,
           alpha-jump, and analog scrolling all share this path so a rapid burst
           never decodes artwork for rows the user only passed through. */
        if (s_pedPending) {
            DWORD now = GetTickCount();
            if ((LONG)(now - s_pedSettleAt) >= 0) {
                LoadPedestal(s_cursor);
                s_pedPending = 0;
            }
        }
        if (pressed & BTN_A) {
            /* Type-D "Now Playing": fire only if a present unit has its art
               toggle on (XL Art and/or Type-D Art). */
            int artOn = TypeDArt_WillSend();
            Audio_PlaySfx(SFX_SELECT);
            /* A can arrive during the short navigation debounce. Ensure the
               loading screen always shows the title that is actually launching. */
            if (s_pedPending) {
                LoadPedestal(s_cursor);
                s_pedPending = 0;
            }
            /* show the loading screen while s_ped art is still loaded, then
               release the pedestal texture, stop the music, and hand off.
               Mount_LaunchXbe does not return on success. */
            if (artOn) DrawLoadingScreen();
            if (s_ped.tex) Texture_Release(&s_ped);
            s_ped.tex = NULL; s_pedIdx = -1; s_pedFlat = 0;
            ReleaseFanart();
            Audio_StopMusic();
            Recents_Add(s_items[s_cursor].label, s_items[s_cursor].xbePath);
            /* Stop the SMBus service thread BEFORE touching the panel. It pages
               the LCD every ~180ms; if it keeps running, its next tick repaints
               the rotating page right over our Now Playing title (and the title
               write invalidates the shadow, guaranteeing that repaint). Stopping
               it (Smbsvc_Stop joins the thread, so no tick is mid-flight) means
               the title we paint below is the last thing on the panel and stays
               up once the game takes over. It also frees the bus so the art send
               runs uncontended. Restarted only if the launch fails (below). */
            Smbsvc_Stop();
            Watchdog_Suspend();
            /* freeze the X-View on a Now Playing frame with this title's
               cover art, then shut the panel USB down leaving that frame up
               while the game runs (mirrors LCD Now Playing persistence). */
            XView_NowPlayingLaunch(s_items[s_cursor].label, s_items[s_cursor].xbePath);
            /* push the cover art to the Type-D (blocking; masked by the loading
               screen already on the front buffer). No-op for pack-less titles.
               Suspend the liveness watchdog across this -- it legitimately stalls
               the main loop for up to several seconds with no present. */
            if (artOn) { TypeDArt_SendArtFor(s_items[s_cursor].xbePath); }
            Watchdog_Resume();
            /* paint the LCD Now Playing screen LAST, right before handoff, so it
               is the final thing written to the panel and persists while the game
               runs. No-op if the LCD accessory is off/absent. */
            Lcd_NowPlaying(s_items[s_cursor].label);
            Mount_LaunchXbe(s_items[s_cursor].xbePath);
            /* fell through -> launch failed. Restart the service thread so sensor
               polling + LCD paging resume, and carry on so the menu stays usable. */
            Smbsvc_Init();
            XView_Start();   /* launch failed -> bring the panel back */
        }
    }
    return 0;
}

/* Fit to virtual pixels, not a fixed character count. This respects the
   variable advances of .ddf fonts without changing the format or atlas. */
static void FitLabelPixels(char* dst, int cap, const char* src, int maxPixels) {
    int n = 0;
    int reserve;
    if (!dst || cap < 1) return;
    dst[0] = 0;
    if (!src || cap < 2 || maxPixels <= 0) return;
    while (src[n] && n < cap - 1) { dst[n] = src[n]; ++n; }
    dst[n] = 0;
    if (!src[n] && Font_MeasureText(dst, FONT_SIZE_SMALL) <= maxPixels) return;
    reserve = Font_MeasureText("..", FONT_SIZE_SMALL);
    if (reserve >= maxPixels || cap < 4) { dst[0] = 0; return; }
    n = 0;
    while (src[n] && n < cap - 3) {
        dst[n] = src[n]; dst[n + 1] = 0;
        if (Font_MeasureText(dst, FONT_SIZE_SMALL) > maxPixels - reserve) break;
        ++n;
    }
    dst[n++] = '.'; dst[n++] = '.'; dst[n] = 0;
}

static int AppendUInt(char* dst, int pos, int cap, unsigned v) {
    char tmp[12]; int n = 0, i;
    if (v == 0) tmp[n++] = '0';
    else while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = n - 1; i >= 0 && pos < cap - 1; i--) dst[pos++] = tmp[i];
    dst[pos] = 0;
    return pos;
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
    const Texture* ped = Theme_Asset("platform_round");

    float menuX = 352.0f, menuY = 48.0f;
    float rowY0 = 86.0f, rowDY = 38.0f;
    const char* title = (s_cfg && s_cfg->title) ? s_cfg->title : "APPLICATIONS";
    int i, visible;

    /* shared ambient bloom (same green wash as the main menu) */
    Backdrop_Draw();
    DrawFanartBackdrop();

    /* header */
    if (hdr) UI_DrawSprite(hdr, 8.0f, 8.0f, 300.0f, 40.0f, 0xFFFFFFFF, 0);
    Font_DrawText(d, 26.0f, 14.0f, title, FONT_SIZE_LARGE, accent, 0);
    if (s_showHidden)
        Font_DrawText(d, 500.0f, 18.0f, "[ HIDDEN ]", FONT_SIZE_SMALL, glow, 0);
    else if (s_count > 0) {
        char pos[24]; int n = 0;
        n = AppendUInt(pos, n, sizeof(pos), (unsigned)(s_cursor + 1));
        if (n < (int)sizeof(pos) - 1) pos[n++] = '/';
        pos[n] = 0;
        AppendUInt(pos, n, sizeof(pos), (unsigned)s_count);
        Font_DrawTextRight(d, 614.0f, 18.0f, pos, FONT_SIZE_SMALL, dim);
    }

    /* pedestal (left): platform base, then the light + title showpiece.
       _resources case art renders as a flat flickering hologram; an XBE title
       image (square) keeps the spinning cube. */
       /* Ambient floor light belongs to the launcher scene, not pedestal logic.
          Two gentle falloff passes sit UNDER the existing authored platform. */
    {
        DWORD c = Theme_StageLighting()->beam;
        int lr = (int)((c >> 16) & 255), lg = (int)((c >> 8) & 255);
        int lb = (int)(c & 255);
        UI_FillGradient(79.0f, 328.0f, 201.0f, 41.0f,
            UI_ARGB(0, lr, lg, lb), UI_ARGB(24, lr, lg, lb),
            UI_ARGB(0, lr, lg, lb), UI_ARGB(15, lr, lg, lb), 0);
    }
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
        Font_DrawTextIsoClip(d, menuX + 24.0f, 192.0f, em,
            FONT_SIZE_SMALL, dim, 224.0f);
        Font_DrawTextIsoClip(d, menuX + 24.0f, 219.0f,
            "Y ADD PATH  /  X RESCAN", FONT_SIZE_SMALL, text, 224.0f);
    }
    else {
        /* same spring/pop/chromatic selection language as the main menu. */
        float gy = rowY0 + rowDY * (float)(s_cursor - s_scroll) - 6.0f;
        Select_Begin(0x2000, gy);
        Select_DrawGlow(menuX + 6.0f, gy, 258.0f, 32.0f, UI_ARGB(80, ar, ag, ab));

        visible = s_count - s_scroll;
        if (visible > LAUNCH_ROWS_VISIBLE) visible = LAUNCH_ROWS_VISIBLE;
        for (i = 0; i < visible; i++) {
            int  idx = s_scroll + i;
            char row[LAUNCH_NAME_MAX + 4];
            int  hid = s_showHidden ? Hidden_Is(s_items[idx].xbePath) : 0;
            DWORD c = (idx == s_cursor) ? glow : (hid ? dim : text);
            if (hid) {
                /* "x " marker so hidden entries are obvious while reverting */
                char tmp[LAUNCH_NAME_MAX + 2];
                int k = 0;
                const int markerWidth = Font_MeasureText("x ", FONT_SIZE_SMALL);
                FitLabelPixels(tmp, sizeof(tmp), s_items[idx].label,
                    markerWidth < 224 ? 224 - markerWidth : 0);
                row[0] = 'x'; row[1] = ' ';
                while (tmp[k] && k < (int)sizeof(row) - 3) { row[2 + k] = tmp[k]; ++k; }
                row[2 + k] = 0;
            }
            else {
                FitLabelPixels(row, sizeof(row), s_items[idx].label, 224);
            }
            Font_DrawTextIsoClip(d, menuX + 24.0f, rowY0 + rowDY * (float)i,
                row, FONT_SIZE_SMALL, c, 224.0f);
        }
    }

    Iso_End();

    /* A subtle, read-only source marker for the selected XBE. Only identify
       known mounted partitions; D: assets, S: optical and custom mappings
       must never be mislabeled as an internal HDD. */
    if (s_count > 0 && s_cursor >= 0 && s_cursor < s_count) {
        const char* xbe = s_items[s_cursor].xbePath;
        if (xbe[0] && xbe[1] == ':') {
            const DD_StorageVolume* vol = DD_Storage_Letter(xbe[0]);
            if (vol && vol->mounted) {
                char source[24];
                source[0] = 0;
                source[0] = 'H'; source[1] = 'D'; source[2] = 'D';
                source[3] = vol->disk ? '1' : '0'; source[4] = ' ';
                source[5] = ' '; source[6] = vol->letter; source[7] = ':'; source[8] = 0;
                Font_DrawText(d, 87.0f, 397.0f, source, FONT_SIZE_SMALL, dim, 0);
            }
        }
    }

    /* footer: expose the useful category controls, but fall back to a compact
       hint with unusually wide custom fonts instead of drawing off the bar. */
    {
        const char* hint;
        const char* compact;
        if (s_count <= 0 && !s_showHidden) {
            hint = "X REFRESH  Y ADD PATH  B BACK";
            compact = hint;
        }
        else if (s_showHidden) {
            hint = "SHOWING HIDDEN   A LAUNCH  BLACK UNHIDE  START EXIT  B BACK";
            compact = "BLACK UNHIDE  START EXIT HIDDEN  B BACK";
        }
        else if (s_cfg && s_cfg->cacheId && lstrcmpiA(s_cfg->cacheId, "games") == 0) {
            hint = "A LAUNCH X REFRESH Y PATH R3 ISO BLACK HIDE START HIDDEN B BACK";
            compact = "A LAUNCH  Y PATH  R3 ISO  X REFRESH  B BACK";
        }
        else {
            hint = "A LAUNCH X REFRESH Y PATH WHITE INFO BLACK HIDE START HIDDEN B BACK";
            compact = "A LAUNCH  Y PATH  WHITE INFO  X REFRESH  B BACK";
        }
        if (DD_Hints_Measure(hint) > 590.0f) hint = compact;
        DD_Hints_Footer(d, 442.0f, 32.0f, 24.0f, 590.0f, hint, text);
    }

    /* overlays on top of everything */
    Browse_Draw(d);
    Synopsis_Draw(d);
    IsoUi_Draw(d);
}