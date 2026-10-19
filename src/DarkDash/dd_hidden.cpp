/*---------------------------------------------------------------------------
    dd_hidden.cpp -- see dd_hidden.h.

    Up to HIDDEN_MAX hidden default.xbe paths, persisted as a versioned blob in
    D:\data\hidden.dat. Version 2 uses the shared DD_PATH_MAX path capacity;
    version-1 (256-byte path) data is migrated transparently.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_hidden.h"
#include "dd_path.h"

#define HIDDEN_MAX          256
#define HIDDEN_PATH_MAX     DD_PATH_MAX
#define HIDDEN_OLD_PATH_MAX 256
#define HIDDEN_MAGIC        0x44494844UL   /* 'DHID' */
#define HIDDEN_VER          2
#define HIDDEN_OLD_VER      1

static char s_paths[HIDDEN_MAX][HIDDEN_PATH_MAX];
static int  s_count = 0;
static int  s_loaded = 0;

/* ---- tiny string helpers (no CRT) -------------------------------------- */

static char LowerC(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static int IEq(const char* a, const char* b) {
    int i = 0;
    for (;;) {
        char ca = LowerC(a[i]), cb = LowerC(b[i]);
        if (ca != cb) return 0;
        if (ca == 0)  return 1;
        i++;
    }
}

static int Save(void);

/* ---- persistence ------------------------------------------------------- */

static void Load(void) {
    HANDLE h;
    DWORD  magic = 0, got = 0;
    int    ver = 0, n = 0;
    int    migrated = 0;

    s_count = 0;
    s_loaded = 1;

    h = CreateFileA("D:\\data\\hidden.dat", GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    if (ReadFile(h, &magic, sizeof(magic), &got, NULL) && got == sizeof(magic) &&
        magic == HIDDEN_MAGIC &&
        ReadFile(h, &ver, sizeof(ver), &got, NULL) && got == sizeof(ver) &&
        ReadFile(h, &n, sizeof(n), &got, NULL) && got == sizeof(n) &&
        n >= 0 && n <= HIDDEN_MAX) {

        if (ver == HIDDEN_VER) {
            DWORD want = (DWORD)n * HIDDEN_PATH_MAX;
            if (n == 0 || (ReadFile(h, s_paths, want, &got, NULL) && got == want)) {
                int i, out = 0;
                for (i = 0; i < n; i++) {
                    s_paths[i][HIDDEN_PATH_MAX - 1] = 0;
                    if (!s_paths[i][0] || !DD_PathIsDosAbsolute(s_paths[i])) continue;
                    if (out != i) DD_PathCopy(s_paths[out], HIDDEN_PATH_MAX, s_paths[i]);
                    out++;
                }
                s_count = out;
            }
        }
        else if (ver == HIDDEN_OLD_VER) {
            int i;
            char oldPath[HIDDEN_OLD_PATH_MAX];
            for (i = 0; i < n; i++) {
                if (!ReadFile(h, oldPath, sizeof(oldPath), &got, NULL) || got != sizeof(oldPath)) {
                    s_count = 0;
                    break;
                }
                oldPath[HIDDEN_OLD_PATH_MAX - 1] = 0;
                if (!oldPath[0] || !DD_PathIsDosAbsolute(oldPath)) continue;
                if (s_count < HIDDEN_MAX && DD_PathCopy(s_paths[s_count], HIDDEN_PATH_MAX, oldPath))
                    s_count++;
            }
            migrated = 1;
        }
    }
    CloseHandle(h);

    if (migrated) Save();              /* best-effort one-time format upgrade */
}

static int Save(void) {
    HANDLE h;
    DWORD  magic = HIDDEN_MAGIC, wr = 0;
    int    ver = HIDDEN_VER;
    DWORD  want = (DWORD)s_count * HIDDEN_PATH_MAX;

    CreateDirectoryA("D:\\data", NULL);
    h = CreateFileA("D:\\data\\hidden.dat", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    WriteFile(h, &magic, sizeof(magic), &wr, NULL);
    WriteFile(h, &ver, sizeof(ver), &wr, NULL);
    WriteFile(h, &s_count, sizeof(s_count), &wr, NULL);
    if (s_count > 0) WriteFile(h, s_paths, want, &wr, NULL);
    CloseHandle(h);
    return 1;
}

/* ---- public API -------------------------------------------------------- */

void Hidden_Init(void) {
    if (!s_loaded) Load();
}

static int IndexOf(const char* xbePath) {
    int i;
    if (!xbePath || !xbePath[0]) return -1;
    for (i = 0; i < s_count; i++)
        if (IEq(s_paths[i], xbePath)) return i;
    return -1;
}

int Hidden_Is(const char* xbePath) {
    if (!s_loaded) Load();
    return (IndexOf(xbePath) >= 0) ? 1 : 0;
}

int Hidden_Set(const char* xbePath, int hide) {
    int idx;
    if (!s_loaded) Load();
    if (!xbePath || !xbePath[0]) return 0;
    if (DD_PathLen(xbePath) >= HIDDEN_PATH_MAX || !DD_PathIsDosAbsolute(xbePath)) return 0;

    idx = IndexOf(xbePath);

    if (hide) {
        if (idx >= 0) return 1;
        if (s_count >= HIDDEN_MAX) return 0;
        if (!DD_PathCopy(s_paths[s_count], HIDDEN_PATH_MAX, xbePath)) return 0;
        s_count++;
    }
    else {
        int k;
        if (idx < 0) return 1;
        for (k = idx; k < s_count - 1; k++)
            DD_PathCopy(s_paths[k], HIDDEN_PATH_MAX, s_paths[k + 1]);
        s_count--;
    }
    return Save();
}

int Hidden_Count(void) {
    if (!s_loaded) Load();
    return s_count;
}