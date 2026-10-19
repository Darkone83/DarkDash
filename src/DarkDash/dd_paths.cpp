/*---------------------------------------------------------------------------
    dd_paths.cpp -- see dd_paths.h.

    Four categories, each with up to DDPATHS_PER_CAT paths. Persisted as a
    simple versioned blob in D:\data\paths.dat. Version 2 widens path slots to
    DD_PATH_MAX; version-1 (120-byte path) files are migrated in place.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_paths.h"

#define DDPATHS_CATS          4
#define DDPATHS_MAGIC         0x44504154UL   /* 'DPAT' */
#define DDPATHS_VER           2
#define DDPATHS_OLD_VER       1
#define DDPATHS_OLD_PATH_MAX  120

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

/* exact v1 on-disk layout, retained only for migration */
typedef struct {
    int  count;
    char path[DDPATHS_PER_CAT][DDPATHS_OLD_PATH_MAX];
} CatPathsV1;

typedef struct {
    DWORD      magic;
    DWORD      version;
    CatPathsV1 cat[DDPATHS_CATS];
} PathsBlobV1;

static PathsBlob s_p;
static int       s_loaded = 0;

/* ---- helpers ----------------------------------------------------------- */

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

static void SanitizeBlob(void) {
    int i, j, out;
    s_p.magic = DDPATHS_MAGIC;
    s_p.version = DDPATHS_VER;
    for (i = 0; i < DDPATHS_CATS; i++) {
        if (s_p.cat[i].count < 0) s_p.cat[i].count = 0;
        if (s_p.cat[i].count > DDPATHS_PER_CAT) s_p.cat[i].count = DDPATHS_PER_CAT;
        out = 0;
        for (j = 0; j < s_p.cat[i].count; j++) {
            s_p.cat[i].path[j][DDPATHS_PATH_MAX - 1] = 0;
            if (!DD_PathIsDosAbsolute(s_p.cat[i].path[j])) continue;
            if (out != j) DD_PathCopy(s_p.cat[i].path[out], DDPATHS_PATH_MAX, s_p.cat[i].path[j]);
            out++;
        }
        s_p.cat[i].count = out;
        while (out < DDPATHS_PER_CAT) s_p.cat[i].path[out++][0] = 0;
    }
}

/* ---- public ------------------------------------------------------------ */

void Paths_Load(void) {
    HANDLE h;
    DWORD  got = 0, sz;
    int    migrated = 0;
    PathsBlob tmp;
    PathsBlobV1 old;

    ResetBlob();
    s_loaded = 1;

    h = CreateFileA("D:\\data\\paths.dat", GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;            /* none yet -> empty */

    sz = GetFileSize(h, NULL);
    if (sz == sizeof(PathsBlob)) {
        if (ReadFile(h, &tmp, sizeof(tmp), &got, NULL) && got == sizeof(tmp) &&
            tmp.magic == DDPATHS_MAGIC && tmp.version == DDPATHS_VER) {
            s_p = tmp;
            SanitizeBlob();
        }
    }
    else if (sz == sizeof(PathsBlobV1)) {
        if (ReadFile(h, &old, sizeof(old), &got, NULL) && got == sizeof(old) &&
            old.magic == DDPATHS_MAGIC && old.version == DDPATHS_OLD_VER) {
            int i, j, count;
            ResetBlob();
            for (i = 0; i < DDPATHS_CATS; i++) {
                count = old.cat[i].count;
                if (count < 0) count = 0;
                if (count > DDPATHS_PER_CAT) count = DDPATHS_PER_CAT;
                for (j = 0; j < count; j++) {
                    old.cat[i].path[j][DDPATHS_OLD_PATH_MAX - 1] = 0;
                    if (DD_PathIsDosAbsolute(old.cat[i].path[j]) &&
                        DD_PathCopy(s_p.cat[i].path[s_p.cat[i].count], DDPATHS_PATH_MAX,
                            old.cat[i].path[j]))
                        s_p.cat[i].count++;
                }
            }
            migrated = 1;
        }
    }
    CloseHandle(h);

    /* Best-effort one-time rewrite in the widened format. */
    if (migrated) Paths_Save();
}

int Paths_Save(void) {
    HANDLE h;
    DWORD  wr = 0;
    if (!s_loaded) { ResetBlob(); s_loaded = 1; }

    SanitizeBlob();
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
    n = DD_PathLen(path);
    if (n <= 0 || n >= DDPATHS_PATH_MAX) return 0;
    if (!DD_PathIsDosAbsolute(path)) return 0;

    /* no duplicates within the category */
    for (j = 0; j < s_p.cat[ci].count; j++)
        if (PEqCI(s_p.cat[ci].path[j], path)) return 0;
    if (s_p.cat[ci].count >= DDPATHS_PER_CAT) return 0;
    if (!DD_PathCopy(s_p.cat[ci].path[s_p.cat[ci].count], DDPATHS_PATH_MAX, path)) return 0;
    s_p.cat[ci].count++;
    return 1;
}

void Paths_Remove(const char* cacheId, int i) {
    int ci, j;
    if (!s_loaded) Paths_Load();
    ci = CatIndex(cacheId);
    if (ci < 0 || i < 0 || i >= s_p.cat[ci].count) return;
    for (j = i; j < s_p.cat[ci].count - 1; j++)
        DD_PathCopy(s_p.cat[ci].path[j], DDPATHS_PATH_MAX, s_p.cat[ci].path[j + 1]);
    s_p.cat[ci].count--;
    s_p.cat[ci].path[s_p.cat[ci].count][0] = 0;
}