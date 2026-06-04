/*---------------------------------------------------------------------------
    dd_paths.cpp -- see dd_paths.h.

    Four categories, each with up to DDPATHS_PER_CAT paths. Persisted as a
    simple versioned blob in D:\data\paths.dat. C89 style, no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_paths.h"

#define DDPATHS_CATS    4
#define DDPATHS_MAGIC   0x44504154UL   /* 'DPAT' */
#define DDPATHS_VER     1

/* fixed category order; index <-> cacheId */
static const char* const k_catKeys[DDPATHS_CATS] = { "apps", "games", "homebrew", "emu" };

typedef struct {
    int  count;
    char path[DDPATHS_PER_CAT][DDPATHS_PATH_MAX];
} CatPaths;

typedef struct {
    DWORD    magic;
    DWORD    version;
    CatPaths cat[DDPATHS_CATS];
} PathsBlob;

static PathsBlob s_p;
static int       s_loaded = 0;

/* ---- helpers ----------------------------------------------------------- */

static int PLen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void PCopy(char* dst, int cap, const char* src) {
    int i = 0; if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int PEqCI(const char* a, const char* b) {
    int i = 0;
    for (;;) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        if (ca == 0) return 1;
        i++;
    }
}

static int CatIndex(const char* cacheId) {
    int i;
    if (!cacheId) return -1;
    for (i = 0; i < DDPATHS_CATS; i++)
        if (PEqCI(cacheId, k_catKeys[i])) return i;
    return -1;
}

static void ResetBlob(void) {
    int i, j;
    s_p.magic = DDPATHS_MAGIC;
    s_p.version = DDPATHS_VER;
    for (i = 0; i < DDPATHS_CATS; i++) {
        s_p.cat[i].count = 0;
        for (j = 0; j < DDPATHS_PER_CAT; j++) s_p.cat[i].path[j][0] = 0;
    }
}

/* ---- public ------------------------------------------------------------ */

void Paths_Load(void) {
    HANDLE h;
    DWORD  got = 0;
    PathsBlob tmp;

    ResetBlob();
    s_loaded = 1;

    h = CreateFileA("D:\\data\\paths.dat", GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;            /* none yet -> empty */
    if (ReadFile(h, &tmp, sizeof(tmp), &got, NULL) && got == sizeof(tmp) &&
        tmp.magic == DDPATHS_MAGIC && tmp.version == DDPATHS_VER) {
        /* clamp counts defensively before trusting the file */
        int i;
        for (i = 0; i < DDPATHS_CATS; i++) {
            if (tmp.cat[i].count < 0) tmp.cat[i].count = 0;
            if (tmp.cat[i].count > DDPATHS_PER_CAT) tmp.cat[i].count = DDPATHS_PER_CAT;
        }
        s_p = tmp;
    }
    CloseHandle(h);
}

int Paths_Save(void) {
    HANDLE h;
    DWORD  wr = 0;
    if (!s_loaded) { ResetBlob(); s_loaded = 1; }

    CreateDirectoryA("D:\\data", NULL);   /* ensure dir exists; ignore exists */
    h = CreateFileA("D:\\data\\paths.dat", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, &s_p, sizeof(s_p), &wr, NULL);
    CloseHandle(h);
    return (wr == sizeof(s_p)) ? 1 : 0;
}

int Paths_Count(const char* cacheId) {
    int ci;
    if (!s_loaded) Paths_Load();
    ci = CatIndex(cacheId);
    if (ci < 0) return 0;
    return s_p.cat[ci].count;
}

const char* Paths_Get(const char* cacheId, int i) {
    int ci;
    if (!s_loaded) Paths_Load();
    ci = CatIndex(cacheId);
    if (ci < 0 || i < 0 || i >= s_p.cat[ci].count) return "";
    return s_p.cat[ci].path[i];
}

int Paths_Add(const char* cacheId, const char* path) {
    int ci, j, n;
    if (!s_loaded) Paths_Load();
    ci = CatIndex(cacheId);
    if (ci < 0 || !path || !path[0]) return 0;
    n = PLen(path);
    if (n >= DDPATHS_PATH_MAX) return 0;
    /* must look like a DOS path "X:\..." */
    if (path[1] != ':') return 0;
    /* no duplicates within the category */
    for (j = 0; j < s_p.cat[ci].count; j++)
        if (PEqCI(s_p.cat[ci].path[j], path)) return 0;
    if (s_p.cat[ci].count >= DDPATHS_PER_CAT) return 0;
    PCopy(s_p.cat[ci].path[s_p.cat[ci].count], DDPATHS_PATH_MAX, path);
    s_p.cat[ci].count++;
    return 1;
}

void Paths_Remove(const char* cacheId, int i) {
    int ci, j;
    if (!s_loaded) Paths_Load();
    ci = CatIndex(cacheId);
    if (ci < 0 || i < 0 || i >= s_p.cat[ci].count) return;
    for (j = i; j < s_p.cat[ci].count - 1; j++)
        PCopy(s_p.cat[ci].path[j], DDPATHS_PATH_MAX, s_p.cat[ci].path[j + 1]);
    s_p.cat[ci].count--;
    s_p.cat[ci].path[s_p.cat[ci].count][0] = 0;
}