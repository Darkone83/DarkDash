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

/* INI values can contain quoted # or ; characters. Strip only comments that
   begin outside quotes at a token boundary. This also preserves #RRGGBB. */
static void ini_clean_value(char* s) {
    int quoted = 0;
    char* p = s;
    while (*p) {
        if ((*p == '\"' || *p == '\'') && (!quoted || quoted == *p)) {
            quoted = quoted ? 0 : *p;
        }
        else if (!quoted && (*p == ';' || *p == '#') &&
            ((*p == ';' && p == s) || (p != s && (p[-1] == ' ' || p[-1] == '\t')))) {
            *p = 0;
            break;
        }
        p++;
    }
    p = trim(s);
    if (p != s) memmove(s, p, strlen(p) + 1);
    {   /* optional matching quotes (paths containing spaces/comments) */
        size_t n = strlen(s);
        if (n >= 2 && (s[0] == '\"' || s[0] == '\'') && s[n - 1] == s[0]) {
            memmove(s, s + 1, n - 2);
            s[n - 2] = 0;
        }
    }
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
                    ini_clean_value(e->val);
                }
            }
        }
    }
}

/* A theme path may contain only relative components, never an absolute drive,
   parent traversal or control character. All joins check for truncation. */
static int relative_path_ok(const char* p) {
    const char* seg;
    const char* c;
    if (!p || !p[0] || p[0] == '\\' || p[0] == '/') return 0;
    seg = p;
    for (c = p; ; ++c) {
        if (*c == ':' || (*c && (unsigned char)*c < 32)) return 0;
        if (*c == '\\' || *c == '/' || *c == 0) {
            int n = (int)(c - seg);
            if (n == 0 || (n == 1 && seg[0] == '.') ||
                (n == 2 && seg[0] == '.' && seg[1] == '.')) return 0;
            if (!*c) break;
            seg = c + 1;
        }
    }
    return 1;
}

static int path_join(char* out, size_t cap, const char* base, const char* relative) {
    size_t b, r, i;
    if (!out || !base || !relative || !relative_path_ok(relative)) return 0;
    b = strlen(base);
    r = strlen(relative);
    if (b + 1 + r + 1 > cap) return 0;
    memcpy(out, base, b);
    out[b++] = '\\';
    for (i = 0; i < r; ++i)
        out[b + i] = (relative[i] == '/') ? '\\' : relative[i];
    out[b + r] = 0;
    return 1;
}

/* Named assets are basenames, not filesystem paths. */
static int asset_name_ok(const char* name) {
    int i;
    if (!name || !name[0] || strlen(name) >= NAME_MAX_DD) return 0;
    for (i = 0; name[i]; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
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
    if (s[6] != 0) return fallback;
    hi = v[0] * 16 + v[1];          /* R */
    lo = v[2] * 16 + v[3];          /* G */
    return (DWORD)((0xFF << 24) | (hi << 16) | (lo << 8) | (v[4] * 16 + v[5]));
}

/* A3: stage-lighting overrides. No requirement for themes to declare these:
   the palette automatically supplies the corresponding visual tints. */
static ThemeStageLighting s_stageLight;

static int ini_percent(const char* section, const char* key, int fallback) {
    const char* p = ini_get(section, key);
    int n = 0;
    if (!p || !*p) return fallback;
    while (*p >= '0' && *p <= '9') {
        if (n > 2000) return fallback;
        n = n * 10 + (*p++ - '0');
    }
    if (*p || n > 200) return fallback;
    return n;
}

static void stage_light_configure(void) {
    const char* m = ini_get("stage.lighting", "material");
    const char* power = ini_get("stage.lighting", "specular_power");
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    int x = 0;
    s_stageLight.material = DD_STAGE_LIQUID;
    if (m) {
        if (lstrcmpiA(m, "hologram") == 0) s_stageLight.material = DD_STAGE_HOLOGRAM;
        else if (lstrcmpiA(m, "metal") == 0) s_stageLight.material = DD_STAGE_METAL;
    }
    s_stageLight.ambient = parse_hex_rgb(ini_get("stage.lighting", "ambient"), 0xFF303030);
    s_stageLight.key = parse_hex_rgb(ini_get("stage.lighting", "key"), 0xFFFFFFFF);
    s_stageLight.emissive = parse_hex_rgb(ini_get("stage.lighting", "emissive"), accent);
    s_stageLight.beam = parse_hex_rgb(ini_get("stage.lighting", "beam"), accent);
    s_stageLight.arcsExplicit = parse_hex_rgb(ini_get("stage.lighting", "arcs"), 0) != 0;
    s_stageLight.arcs = parse_hex_rgb(ini_get("stage.lighting", "arcs"), accent);

    /* Profiles define material character only; missing keys always resolve to
       the original liquid settings or their theme-specific profile defaults. */
    s_stageLight.ambientLevel = ini_percent("stage.lighting", "ambient_level", 100);
    s_stageLight.keyLevel = ini_percent("stage.lighting", "key_level", 100);
    s_stageLight.emissiveLevel = ini_percent("stage.lighting", "emissive_level",
        s_stageLight.material == DD_STAGE_HOLOGRAM ? 155 : (s_stageLight.material == DD_STAGE_METAL ? 45 : 100));
    s_stageLight.specularLevel = ini_percent("stage.lighting", "specular_level",
        s_stageLight.material == DD_STAGE_METAL ? 145 : (s_stageLight.material == DD_STAGE_HOLOGRAM ? 45 : 100));
    s_stageLight.beamLevel = ini_percent("stage.lighting", "beam_level", 100);
    s_stageLight.arcLevel = ini_percent("stage.lighting", "arc_level",
        s_stageLight.material == DD_STAGE_METAL ? 35 : 100);
    s_stageLight.motionLevel = ini_percent("stage.lighting", "motion_level",
        s_stageLight.material == DD_STAGE_METAL ? 55 : (s_stageLight.material == DD_STAGE_HOLOGRAM ? 65 : 100));
    s_stageLight.pulseLevel = ini_percent("stage.lighting", "pulse_level",
        s_stageLight.material == DD_STAGE_METAL ? 40 : 100);
    s_stageLight.specularPower = s_stageLight.material == DD_STAGE_METAL ? 46 :
        (s_stageLight.material == DD_STAGE_HOLOGRAM ? 30 : 20);
    if (power && *power) {
        for (; *power >= '0' && *power <= '9'; ++power) {
            if (x > 1000) { x = 0; break; }
            x = x * 10 + (*power - '0');
        }
        if (!*power && x >= 2 && x <= 96) s_stageLight.specularPower = x;
    }
}

const ThemeStageLighting* Theme_StageLighting(void) {
    return &s_stageLight;
}

/*    Public                                                                  */

int Theme_Load(const char* themeRoot) {
    char path[PATH_MAX_DD];
    unsigned char* buf = NULL;
    size_t sz = 0;
    const char* adir;

    Theme_Unload();
    if (!themeRoot || !themeRoot[0] || strlen(themeRoot) >= sizeof(s_root)) return 0;

    strcpy(s_root, themeRoot);
    if (!path_join(path, sizeof(path), s_root, "theme.ini")) {
        Theme_Unload();
        return 0;
    }

    if (Fileops_LoadFile(&buf, &sz, path) != 0 || !buf || sz == 0) {
        if (buf) free(buf);
        Theme_Unload();
        return 0;
    }

    {   /* null-terminate the loaded text */
        char* text = (char*)malloc(sz + 1);
        if (!text) { free(buf); Theme_Unload(); return 0; }
        memcpy(text, buf, sz);
        text[sz] = 0;
        free(buf);
        ini_clear();
        ini_parse(text);
        free(text);
    }

    /* Validate relative asset_dir. Invalid settings fall back to assets. */
    adir = ini_get("manifest", "asset_dir");
    if (!relative_path_ok(adir)) adir = "assets";
    if (!path_join(s_asset_dir, sizeof(s_asset_dir), s_root, adir)) {
        Theme_Unload();
        return 0;
    }
    stage_light_configure();
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
    /* Empty/failed loads also return a safe legacy lighting configuration. */
    memset(&s_stageLight, 0, sizeof(s_stageLight));
    s_stageLight.material = DD_STAGE_LIQUID;
    s_stageLight.ambient = 0xFF303030;
    s_stageLight.key = 0xFFFFFFFF;
    s_stageLight.emissive = s_stageLight.beam = s_stageLight.arcs = 0xFF7FE000;
    s_stageLight.ambientLevel = s_stageLight.keyLevel = 100;
    s_stageLight.emissiveLevel = s_stageLight.specularLevel = 100;
    s_stageLight.beamLevel = s_stageLight.arcLevel = 100;
    s_stageLight.motionLevel = s_stageLight.pulseLevel = 100;
    s_stageLight.specularPower = 20;
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

    if (!asset_name_ok(name) || !s_asset_dir[0]) return NULL;

    for (i = 0; i < s_cache_n; i++) {
        if (s_cache[i].used && strcmp(s_cache[i].name, name) == 0)
            return s_cache[i].tex.tex ? &s_cache[i].tex : NULL;
    }
    if (s_cache_n >= CACHE_MAX) return NULL;

    {   /* keep the extension separate so the final path is length-checked */
        char png[NAME_MAX_DD + 5];
        strcpy(png, name);
        strcat(png, ".png");
        if (!path_join(path, sizeof(path), s_asset_dir, png)) return NULL;
    }

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

/* Only approved visual roles are interpreted. This is NOT a free-form layout
   engine; unknown roles return NULL. [assets] overrides missing images fall
   back to the established named asset for that role. */
const Texture* Theme_RoleAsset(const char* role) {
    static const struct { const char* role; const char* fallback; } roles[] = {
        { "ui.menu.frame", "frame_menu_v" },
        { "ui.header.frame", "bar_header" },
        { "ui.footer.frame", "bar_footer" },
        { "ui.selection.glow", "overlay_selection_glow" },
        { "stage.orb.body", "orb_hero" },
        { "stage.platform", "platform_round" },
        { "stage.orb.glow", NULL } /* optional emissive mask, A3 */
    };
    unsigned i;
    if (!role) return NULL;
    for (i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
        if (strcmp(role, roles[i].role) == 0) {
            const char* overrideName = ini_get("assets", role);
            const Texture* tex;
            if (overrideName && (strcmp(overrideName, "none") == 0 ||
                strcmp(overrideName, "off") == 0)) return NULL;
            if (overrideName && asset_name_ok(overrideName)) {
                tex = Theme_Asset(overrideName);
                if (tex) return tex;
            }
            return roles[i].fallback ? Theme_Asset(roles[i].fallback) : NULL;
        }
    }
    return NULL;
}

/* Resolve a raw icon file from the configured asset_dir, with a legacy
   default-theme raw fallback for missing icons. */
void Theme_ResolveIcon(const char* name, char* out, int cap) {
    char path[PATH_MAX_DD];
    char rel[PATH_MAX_DD];
    DWORD attr;
    size_t len;

    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!name || !name[0]) return;
    /* Icon filenames may include .png, but cannot escape the raw directory. */
    len = strlen(name);
    if (len >= NAME_MAX_DD || !relative_path_ok(name) ||
        strchr(name, '/') || strchr(name, '\\')) return;

    /* 1) active theme's configured asset_dir\raw */
    if (s_asset_dir[0]) {
        if (path_join(path, sizeof(path), s_asset_dir, "raw") &&
            path_join(rel, sizeof(rel), path, name)) {
            attr = GetFileAttributesA(rel);
            if (attr != 0xFFFFFFFF && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                if (strlen(rel) < (size_t)cap) strcpy(out, rel);
                return;
            }
        }
    }

    /* 2) original default theme fallback is preserved for legacy installations. */
    if (path_join(path, sizeof(path), "D:\\themes\\default\\assets\\raw", name)) {
        if (strlen(path) < (size_t)cap) strcpy(out, path);
    }
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
    /* keep default pinned first when present; sort custom themes alphabetically */
    {
        int first = (s_themeCount > 0 && lstrcmpiA(s_themeNames[0], "default") == 0) ? 1 : 0;
        for (i = first + 1; i < s_themeCount; i++) {
            char key[THEME_NAME_MAX];
            int j = i - 1;
            strncpy(key, s_themeNames[i], THEME_NAME_MAX - 1);
            key[THEME_NAME_MAX - 1] = 0;
            while (j >= first && lstrcmpiA(s_themeNames[j], key) > 0) {
                strncpy(s_themeNames[j + 1], s_themeNames[j], THEME_NAME_MAX - 1);
                s_themeNames[j + 1][THEME_NAME_MAX - 1] = 0;
                j--;
            }
            strncpy(s_themeNames[j + 1], key, THEME_NAME_MAX - 1);
            s_themeNames[j + 1][THEME_NAME_MAX - 1] = 0;
        }
    }
    return s_themeCount;
}

int Theme_Count(void) { return s_themeCount; }

const char* Theme_NameAt(int idx) {
    if (idx < 0 || idx >= s_themeCount) return NULL;
    return s_themeNames[idx];
}

void Theme_RootFor(const char* themesRoot, const char* name, char* out, int cap) {
    size_t len;
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!themesRoot || !name || !name[0] || strchr(name, '/') ||
        strchr(name, '\\') || !relative_path_ok(name)) return;
    len = strlen(themesRoot) + 1 + strlen(name) + 1;
    if (len > (size_t)cap) return;  /* never return a truncated path */
    if (!path_join(out, (size_t)cap, themesRoot, name)) out[0] = 0;
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

/* Optional existing [background].parallax; accept only finite values in
   0..1, and reject trailing junk. A zero value retains the static backdrop. */
float Theme_BackgroundParallax(void) {
    const char* v = ini_get("background", "parallax");
    char* end;
    double n;
    if (!v || !*v) return 0.0f;
    n = strtod(v, &end);
    if (end == v || *end || !(n >= 0.0 && n <= 1.0)) return 0.0f;
    return (float)n;
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

    /* Missing/invalid optional images fall back to procedural atmosphere. */
    if (!path_join(path, sizeof(path), s_root, image)) return NULL;
    {
        DWORD attr = GetFileAttributesA(path);
        if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    }
    if (!Texture_LoadPNG(path, &s_bgImage)) {
        s_bgImage.tex = NULL;
        s_bgOk = 0;
        return NULL;
    }
    s_bgOk = 1;
    return &s_bgImage;
}