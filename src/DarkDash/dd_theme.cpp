/*---------------------------------------------------------------------------
    dd_theme.cpp -- theme.ini parse + palette + lazy asset cache.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <stdlib.h>
#include <string.h>
#include "dd_theme.h"
#include "dd_texture.h"
#include "dd_fileops.h"  /* Fileops_LoadFile: generic file -> buffer */

#define UI_BG_DEFAULT 0xFF060A06   /* matches palette bg 060A06 */
#define INI_MAX     128
#define CACHE_MAX   48
#define PATH_MAX_DD 272
#define NAME_MAX_DD 40

typedef struct { char sect[32]; char key[40]; char val[96]; } IniEntry;

static IniEntry s_ini[INI_MAX];
static int      s_ini_n = 0;

static char     s_root[PATH_MAX_DD] = { 0 };
static char     s_asset_dir[PATH_MAX_DD] = { 0 };

typedef struct { char name[NAME_MAX_DD]; Texture tex; int used; } CacheSlot;
static CacheSlot s_cache[CACHE_MAX];
static int       s_cache_n = 0;

/* discovered theme names (sub-folders of the themes root with a theme.ini) */
#define THEME_LIST_MAX 64
static char s_themeNames[THEME_LIST_MAX][THEME_NAME_MAX];
static int  s_themeCount = 0;

/* a painted background, loaded on demand from [background] image */
static Texture s_bgImage;
static int     s_bgTried = 0;   /* attempted load? */
static int     s_bgOk = 0;   /* load succeeded?  */

/*    INI parsing                                                             */

static char* trim(char* s) {
    char* e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
        e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void ini_clear(void) { s_ini_n = 0; }

static void ini_parse(char* text) {
    char sect[32];
    char* line;
    char* next;
    sect[0] = 0;

    for (line = text; line && *line; line = next) {
        char* nl = strchr(line, '\n');
        if (nl) { *nl = 0; next = nl + 1; }
        else next = NULL;

        {
            char* t = trim(line);
            if (t[0] == 0 || t[0] == ';' || t[0] == '#') continue;

            if (t[0] == '[') {
                char* close = strchr(t, ']');
                if (close) {
                    *close = 0;
                    strncpy(sect, t + 1, sizeof(sect) - 1);
                    sect[sizeof(sect) - 1] = 0;
                }
                continue;
            }

            {
                char* eq = strchr(t, '=');
                if (!eq) continue;
                *eq = 0;
                if (s_ini_n < INI_MAX) {
                    IniEntry* e = &s_ini[s_ini_n++];
                    strncpy(e->sect, sect, sizeof(e->sect) - 1); e->sect[sizeof(e->sect) - 1] = 0;
                    strncpy(e->key, trim(t), sizeof(e->key) - 1); e->key[sizeof(e->key) - 1] = 0;
                    strncpy(e->val, trim(eq + 1), sizeof(e->val) - 1); e->val[sizeof(e->val) - 1] = 0;
                }
            }
        }
    }
}

static const char* ini_get(const char* sect, const char* key) {
    int i;
    for (i = 0; i < s_ini_n; i++) {
        if (strcmp(s_ini[i].sect, sect) == 0 && strcmp(s_ini[i].key, key) == 0)
            return s_ini[i].val;
    }
    return NULL;
}

/*    Colour parse                                                            */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* parse "RRGGBB" -> ARGB with full alpha */
static DWORD parse_hex_rgb(const char* s, DWORD fallback) {
    int v[6], i, hi, lo;
    if (!s) return fallback;
    if (*s == '#') s++;
    for (i = 0; i < 6; i++) {
        v[i] = hexval(s[i]);
        if (v[i] < 0) return fallback;
    }
    hi = v[0] * 16 + v[1];          /* R */
    lo = v[2] * 16 + v[3];          /* G */
    return (DWORD)((0xFF << 24) | (hi << 16) | (lo << 8) | (v[4] * 16 + v[5]));
}

/*    Public                                                                  */

int Theme_Load(const char* themeRoot) {
    char path[PATH_MAX_DD];
    unsigned char* buf = NULL;
    size_t sz = 0;
    const char* adir;

    Theme_Unload();
    if (!themeRoot) return 0;

    strncpy(s_root, themeRoot, sizeof(s_root) - 1);
    s_root[sizeof(s_root) - 1] = 0;

    strncpy(path, s_root, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    strncat(path, "\\theme.ini", sizeof(path) - strlen(path) - 1);

    if (Fileops_LoadFile(&buf, &sz, path) != 0 || !buf || sz == 0) {
        if (buf) free(buf);
        return 0;
    }

    {   /* null-terminate the loaded text */
        char* text = (char*)malloc(sz + 1);
        if (!text) { free(buf); return 0; }
        memcpy(text, buf, sz);
        text[sz] = 0;
        free(buf);
        ini_clear();
        ini_parse(text);
        free(text);
    }

    /* asset dir: <root>\<asset_dir> (default "assets") */
    adir = ini_get("manifest", "asset_dir");
    if (!adir) adir = "assets";
    strncpy(s_asset_dir, s_root, sizeof(s_asset_dir) - 1);
    s_asset_dir[sizeof(s_asset_dir) - 1] = 0;
    strncat(s_asset_dir, "\\", sizeof(s_asset_dir) - strlen(s_asset_dir) - 1);
    strncat(s_asset_dir, adir, sizeof(s_asset_dir) - strlen(s_asset_dir) - 1);
    return 1;
}

void Theme_Unload(void) {
    int i;
    for (i = 0; i < s_cache_n; i++) {
        if (s_cache[i].used) Texture_Release(&s_cache[i].tex);
    }
    s_cache_n = 0;
    s_ini_n = 0;
    s_root[0] = 0;
    s_asset_dir[0] = 0;
    if (s_bgOk) Texture_Release(&s_bgImage);
    s_bgImage.tex = NULL;
    s_bgTried = 0;
    s_bgOk = 0;
}

DWORD Theme_Color(const char* key, DWORD fallback) {
    return parse_hex_rgb(ini_get("palette", key), fallback);
}

DWORD Theme_BG(void) {
    return Theme_Color("bg", UI_BG_DEFAULT);
}

const Texture* Theme_Asset(const char* name) {
    char path[PATH_MAX_DD];
    int i;
    CacheSlot* slot;

    if (!name || !s_asset_dir[0]) return NULL;

    for (i = 0; i < s_cache_n; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].name, name) == 0)
            return s_cache[i].tex.tex ? &s_cache[i].tex : NULL;
    }
    if (s_cache_n >= CACHE_MAX) return NULL;

    strncpy(path, s_asset_dir, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    strncat(path, "\\", sizeof(path) - strlen(path) - 1);
    strncat(path, name, sizeof(path) - strlen(path) - 1);
    strncat(path, ".png", sizeof(path) - strlen(path) - 1);

    slot = &s_cache[s_cache_n++];
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = 0;
    slot->used = 1;

    if (!Texture_LoadPNG(path, &slot->tex)) {
        slot->tex.tex = NULL;   /* cache the miss so we don't retry every frame */
        return NULL;
    }
    return &slot->tex;
}

/* Resolve a raw icon file, preferring the active theme then falling back to
   default. Icons live under "<themeRoot>\assets\raw\<name>". */
void Theme_ResolveIcon(const char* name, char* out, int cap) {
    char path[PATH_MAX_DD];
    DWORD attr;
    int n;

    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!name || !name[0]) return;

    /* 1) active theme's assets\raw, if a theme is loaded */
    if (s_root[0]) {
        n = 0;
        strncpy(path, s_root, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
        strncat(path, "\\assets\\raw\\", sizeof(path) - strlen(path) - 1);
        strncat(path, name, sizeof(path) - strlen(path) - 1);
        { int i; for (i = 0; path[i]; i++) if (path[i] == '/') path[i] = '\\'; }
        attr = GetFileAttributesA(path);
        if (attr != 0xFFFFFFFF && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            strncpy(out, path, cap - 1); out[cap - 1] = 0;
            return;
        }
        (void)n;
    }

    /* 2) fall back to the default theme's icons */
    strncpy(path, "D:\\themes\\default\\assets\\raw\\", sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    strncat(path, name, sizeof(path) - strlen(path) - 1);
    strncpy(out, path, cap - 1); out[cap - 1] = 0;
}

/*    Theme discovery                                                         */

/* does <themesRoot>\<name>\theme.ini exist? */
static int has_theme_ini(const char* themesRoot, const char* name) {
    char path[PATH_MAX_DD];
    DWORD attr;
    strncpy(path, themesRoot, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    strncat(path, "\\", sizeof(path) - strlen(path) - 1);
    strncat(path, name, sizeof(path) - strlen(path) - 1);
    strncat(path, "\\theme.ini", sizeof(path) - strlen(path) - 1);
    attr = GetFileAttributesA(path);
    return (attr != 0xFFFFFFFF && !(attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

int Theme_Scan(const char* themesRoot) {
    char pattern[PATH_MAX_DD];
    WIN32_FIND_DATA fd;
    HANDLE h;
    int i;

    s_themeCount = 0;
    if (!themesRoot || !themesRoot[0]) return 0;

    /* "default" pinned first if it has a theme.ini */
    if (has_theme_ini(themesRoot, "default")) {
        strncpy(s_themeNames[s_themeCount], "default", THEME_NAME_MAX - 1);
        s_themeNames[s_themeCount][THEME_NAME_MAX - 1] = 0;
        s_themeCount++;
    }

    strncpy(pattern, themesRoot, sizeof(pattern) - 1); pattern[sizeof(pattern) - 1] = 0;
    strncat(pattern, "\\*", sizeof(pattern) - strlen(pattern) - 1);

    h = FindFirstFile(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int dup = 0;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.') continue;             /* . and .. */
            if (lstrcmpiA(fd.cFileName, "default") == 0) continue; /* already pinned */
            if (!has_theme_ini(themesRoot, fd.cFileName)) continue;
            for (i = 0; i < s_themeCount; i++)
                if (lstrcmpiA(s_themeNames[i], fd.cFileName) == 0) { dup = 1; break; }
            if (dup) continue;
            if (s_themeCount >= THEME_LIST_MAX) break;
            strncpy(s_themeNames[s_themeCount], fd.cFileName, THEME_NAME_MAX - 1);
            s_themeNames[s_themeCount][THEME_NAME_MAX - 1] = 0;
            s_themeCount++;
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    return s_themeCount;
}

int Theme_Count(void) { return s_themeCount; }

const char* Theme_NameAt(int idx) {
    if (idx < 0 || idx >= s_themeCount) return NULL;
    return s_themeNames[idx];
}

void Theme_RootFor(const char* themesRoot, const char* name, char* out, int cap) {
    int n;
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!themesRoot || !name) return;
    strncpy(out, themesRoot, cap - 1); out[cap - 1] = 0;
    n = (int)strlen(out);
    if (n < cap - 1) { out[n++] = '\\'; out[n] = 0; }
    strncat(out, name, cap - strlen(out) - 1);
}

/*    Glow (themeable highlight)                                              */

int Theme_GlowEnabled(void) {
    const char* v = ini_get("glow", "enabled");
    if (!v) return 1;                 /* default on */
    return (v[0] == '0') ? 0 : 1;
}

DWORD Theme_GlowColor(void) {
    /* [glow] color, else fall back to palette.glow, else a sane default */
    const char* v = ini_get("glow", "color");
    if (v) return parse_hex_rgb(v, 0xFFAEFF3C);
    return Theme_Color("glow", 0xFFAEFF3C);
}

int Theme_GlowIntensity(void) {
    const char* v = ini_get("glow", "intensity");
    int n = 0;
    if (!v) return 100;
    while (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; }
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    return n;
}

/*    Painted background                                                      */

const Texture* Theme_BackgroundImage(void) {
    const char* module;
    const char* image;
    char path[PATH_MAX_DD];

    if (s_bgTried) return s_bgOk ? &s_bgImage : NULL;
    s_bgTried = 1;

    module = ini_get("background", "module");
    if (module && lstrcmpiA(module, "static") != 0) return NULL;  /* only static for now */

    image = ini_get("background", "image");
    if (!image || !image[0] || !s_root[0]) return NULL;

    /* image path is relative to the theme root (e.g. "assets/bg.png") */
    strncpy(path, s_root, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    strncat(path, "\\", sizeof(path) - strlen(path) - 1);
    strncat(path, image, sizeof(path) - strlen(path) - 1);
    {   /* normalize forward slashes to backslashes for the kernel */
        int i; for (i = 0; path[i]; i++) if (path[i] == '/') path[i] = '\\';
    }

    if (!Texture_LoadPNG(path, &s_bgImage)) {
        s_bgImage.tex = NULL;
        s_bgOk = 0;
        return NULL;
    }
    s_bgOk = 1;
    return &s_bgImage;
}