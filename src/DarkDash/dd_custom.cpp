/*---------------------------------------------------------------------------
    dd_custom.cpp -- see dd_custom.h.

    custom.dat is a single fixed blob: { magic, version, count, nextId,
    rec[CUSTOM_MAX] } where each rec is { name, path, cacheId }. Version 2
    widens path storage to DD_PATH_MAX; version-1 (120-byte path) data is
    migrated automatically so existing categories survive the upgrade.

    Build: MSVC2003 / C89 style; Win32/Xbox file API; no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_custom.h"

#define CUSTOM_MAGIC          0x31434344u   /* 'D','C','C','1' */
#define CUSTOM_VER            2
#define CUSTOM_OLD_VER        1
#define CUSTOM_OLD_PATH_MAX   120
#define CUSTOM_FILE           "D:\\data\\custom.dat"
#define CUSTOM_DIR            "D:\\data"

typedef struct {
    char name[CUSTOM_NAME_MAX];
    char path[CUSTOM_PATH_MAX];
    char cacheId[CUSTOM_ID_MAX];
} CustomRec;

typedef struct {
    DWORD     magic;
    DWORD     version;
    DWORD     count;
    DWORD     nextId;
    CustomRec rec[CUSTOM_MAX];
} CustomBlob;

/* exact v1 on-disk layout, retained only for migration */
typedef struct {
    char name[CUSTOM_NAME_MAX];
    char path[CUSTOM_OLD_PATH_MAX];
    char cacheId[CUSTOM_ID_MAX];
} CustomRecV1;

typedef struct {
    DWORD       magic;
    DWORD       version;
    DWORD       count;
    DWORD       nextId;
    CustomRecV1 rec[CUSTOM_MAX];
} CustomBlobV1;

static CustomBlob     s_blob;
static int            s_loaded = 0;
static const char* s_rootPtr[CUSTOM_MAX];   /* each category's 1-element roots[] */
static LauncherConfig s_cfg[CUSTOM_MAX];

static void CCopy(char* d, int cap, const char* s) {
    int i = 0;
    if (cap <= 0) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int CLen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void ResetBlob(void) {
    int i;
    s_blob.magic = CUSTOM_MAGIC;
    s_blob.version = CUSTOM_VER;
    s_blob.count = 0;
    s_blob.nextId = 0;
    for (i = 0; i < CUSTOM_MAX; i++) {
        s_blob.rec[i].name[0] = 0;
        s_blob.rec[i].path[0] = 0;
        s_blob.rec[i].cacheId[0] = 0;
    }
}

/* rebuild the runtime LauncherConfigs to point at the current blob records */
static void Rebuild(void) {
    int i;
    if (s_blob.count > (DWORD)CUSTOM_MAX) s_blob.count = CUSTOM_MAX;
    for (i = 0; i < (int)s_blob.count; i++) {
        s_blob.rec[i].name[CUSTOM_NAME_MAX - 1] = 0;
        s_blob.rec[i].path[CUSTOM_PATH_MAX - 1] = 0;
        s_blob.rec[i].cacheId[CUSTOM_ID_MAX - 1] = 0;
        s_rootPtr[i] = s_blob.rec[i].path;
        s_cfg[i].title = s_blob.rec[i].name;
        s_cfg[i].emptyMsg = "Nothing here yet";
        s_cfg[i].roots = (const char* const*)&s_rootPtr[i];
        s_cfg[i].rootCount = 1;
        s_cfg[i].cacheId = s_blob.rec[i].cacheId;
    }
}

void Custom_Load(void) {
    HANDLE h;
    DWORD  got = 0, sz;
    int    migrated = 0;
    CustomBlob tmp;
    CustomBlobV1 old;

    s_loaded = 1;
    ResetBlob();
    h = CreateFileA(CUSTOM_FILE, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        sz = GetFileSize(h, NULL);
        if (sz == sizeof(CustomBlob)) {
            if (ReadFile(h, &tmp, sizeof(tmp), &got, NULL) && got == sizeof(tmp) &&
                tmp.magic == CUSTOM_MAGIC && tmp.version == CUSTOM_VER &&
                tmp.count <= (DWORD)CUSTOM_MAX) {
                int i, out = 0;
                s_blob = tmp;
                for (i = 0; i < (int)s_blob.count; i++) {
                    s_blob.rec[i].name[CUSTOM_NAME_MAX - 1] = 0;
                    s_blob.rec[i].path[CUSTOM_PATH_MAX - 1] = 0;
                    s_blob.rec[i].cacheId[CUSTOM_ID_MAX - 1] = 0;
                    if (!s_blob.rec[i].name[0] || !s_blob.rec[i].cacheId[0] ||
                        !DD_PathIsDosAbsolute(s_blob.rec[i].path)) continue;
                    if (out != i) s_blob.rec[out] = s_blob.rec[i];
                    out++;
                }
                s_blob.count = (DWORD)out;
            }
        }
        else if (sz == sizeof(CustomBlobV1)) {
            if (ReadFile(h, &old, sizeof(old), &got, NULL) && got == sizeof(old) &&
                old.magic == CUSTOM_MAGIC && old.version == CUSTOM_OLD_VER &&
                old.count <= (DWORD)CUSTOM_MAX) {
                int i;
                ResetBlob();
                s_blob.nextId = old.nextId;
                for (i = 0; i < (int)old.count; i++) {
                    old.rec[i].name[CUSTOM_NAME_MAX - 1] = 0;
                    old.rec[i].path[CUSTOM_OLD_PATH_MAX - 1] = 0;
                    old.rec[i].cacheId[CUSTOM_ID_MAX - 1] = 0;
                    if (!old.rec[i].name[0] || !old.rec[i].cacheId[0] ||
                        !DD_PathIsDosAbsolute(old.rec[i].path)) continue;
                    CCopy(s_blob.rec[s_blob.count].name, CUSTOM_NAME_MAX, old.rec[i].name);
                    DD_PathCopy(s_blob.rec[s_blob.count].path, CUSTOM_PATH_MAX, old.rec[i].path);
                    CCopy(s_blob.rec[s_blob.count].cacheId, CUSTOM_ID_MAX, old.rec[i].cacheId);
                    s_blob.count++;
                }
                migrated = 1;
            }
        }
        CloseHandle(h);
    }
    Rebuild();
    if (migrated) Custom_Save();       /* best-effort one-time format upgrade */
}

int Custom_Save(void) {
    HANDLE h;
    DWORD  wrote = 0;
    int    ok = 0;

    s_blob.magic = CUSTOM_MAGIC;
    s_blob.version = CUSTOM_VER;
    CreateDirectoryA(CUSTOM_DIR, NULL);
    h = CreateFileA(CUSTOM_FILE, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (WriteFile(h, &s_blob, sizeof(s_blob), &wrote, NULL) && wrote == sizeof(s_blob)) ok = 1;
        CloseHandle(h);
    }
    return ok;
}

int Custom_Count(void) {
    if (!s_loaded) Custom_Load();
    return (int)s_blob.count;
}

const LauncherConfig* Custom_Get(int i) {
    if (!s_loaded) Custom_Load();
    if (i < 0 || i >= (int)s_blob.count) return 0;
    return &s_cfg[i];
}

/* cacheId = "cust" + decimal id (filesystem-safe, unique while present) */
static void MakeCacheId(char* out, int cap, DWORD id) {
    char digits[12];
    int  n = 0, o = 0, k;
    const char* pre = "cust";
    DWORD v = id;

    if (cap <= 0) return;
    out[0] = 0;
    if (v == 0) digits[n++] = '0';
    else while (v && n < 11) { digits[n++] = (char)('0' + (v % 10)); v /= 10; }

    while (pre[o] && o < cap - 1) { out[o] = pre[o]; o++; }
    for (k = n - 1; k >= 0 && o < cap - 1; k--) out[o++] = digits[k];
    out[o] = 0;
}

int Custom_Add(const char* name, const char* path) {
    int idx;
    if (!s_loaded) Custom_Load();
    if (!name || !name[0] || !path || !path[0]) return 0;
    if (s_blob.count >= (DWORD)CUSTOM_MAX) return 0;
    if (CLen(name) >= CUSTOM_NAME_MAX) return 0;
    if (DD_PathLen(path) >= CUSTOM_PATH_MAX || !DD_PathIsDosAbsolute(path)) return 0;

    idx = (int)s_blob.count;
    CCopy(s_blob.rec[idx].name, CUSTOM_NAME_MAX, name);
    if (!DD_PathCopy(s_blob.rec[idx].path, CUSTOM_PATH_MAX, path)) return 0;
    MakeCacheId(s_blob.rec[idx].cacheId, CUSTOM_ID_MAX, s_blob.nextId);
    s_blob.nextId++;
    s_blob.count++;
    Rebuild();
    Custom_Save();
    return 1;
}

int Custom_Remove(int i) {
    int k;
    if (!s_loaded) Custom_Load();
    if (i < 0 || i >= (int)s_blob.count) return 0;
    for (k = i; k < (int)s_blob.count - 1; k++) s_blob.rec[k] = s_blob.rec[k + 1];
    s_blob.count--;
    Rebuild();
    Custom_Save();
    return 1;
}