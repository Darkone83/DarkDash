/*---------------------------------------------------------------------------
    dd_hidden.cpp -- see dd_hidden.h.

    Up to HIDDEN_MAX hidden default.xbe paths, persisted as a versioned blob in
    D:\data\hidden.dat. C89 style, no CRT str*. Mirrors the dd_paths approach.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_hidden.h"

#define HIDDEN_MAX        256
#define HIDDEN_PATH_MAX   256
#define HIDDEN_MAGIC      0x44494844UL   /* 'DHID' */
#define HIDDEN_VER        1

static char s_paths[HIDDEN_MAX][HIDDEN_PATH_MAX];
static int  s_count = 0;
static int  s_loaded = 0;

/* ---- tiny string helpers (no CRT) -------------------------------------- */

static char LowerC(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

/* case-insensitive equality */
static int IEq(const char* a, const char* b) {
    int i = 0;
    for (;;) {
        char ca = LowerC(a[i]), cb = LowerC(b[i]);
        if (ca != cb) return 0;
        if (ca == 0)  return 1;
        i++;
    }
}

/* bounded copy into a HIDDEN_PATH_MAX slot */
static void CopyPath(char* dst, const char* src) {
    int i = 0;
    while (src[i] && i < HIDDEN_PATH_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* ---- persistence ------------------------------------------------------- */

static void Load(void) {
    HANDLE h;
    DWORD  magic = 0, got = 0;
    int    ver = 0, n = 0;

    s_count = 0;
    s_loaded = 1;

    h = CreateFileA("D:\\data\\hidden.dat", GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;       /* none yet -> empty set */

    if (ReadFile(h, &magic, sizeof(magic), &got, NULL) && got == sizeof(magic) &&
        magic == HIDDEN_MAGIC &&
        ReadFile(h, &ver, sizeof(ver), &got, NULL) && got == sizeof(ver) &&
        ver == HIDDEN_VER &&
        ReadFile(h, &n, sizeof(n), &got, NULL) && got == sizeof(n) &&
        n >= 0 && n <= HIDDEN_MAX) {
        DWORD want = (DWORD)n * HIDDEN_PATH_MAX;
        if (n == 0 || (ReadFile(h, s_paths, want, &got, NULL) && got == want))
            s_count = n;
    }
    CloseHandle(h);
}

static int Save(void) {
    HANDLE h;
    DWORD  magic = HIDDEN_MAGIC, wr = 0;
    int    ver = HIDDEN_VER;
    DWORD  want = (DWORD)s_count * HIDDEN_PATH_MAX;

    CreateDirectoryA("D:\\data", NULL);          /* ensure dir; ignore exists */
    h = CreateFileA("D:\\data\\hidden.dat", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;      /* read-only volume -> skip */

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

    idx = IndexOf(xbePath);

    if (hide) {
        if (idx >= 0) return 1;                  /* already hidden */
        if (s_count >= HIDDEN_MAX) return 0;     /* full */
        CopyPath(s_paths[s_count], xbePath);
        s_count++;
    }
    else {
        int k;
        if (idx < 0) return 1;                   /* already visible */
        for (k = idx; k < s_count - 1; k++)
            CopyPath(s_paths[k], s_paths[k + 1]);
        s_count--;
    }
    return Save();
}

int Hidden_Count(void) {
    if (!s_loaded) Load();
    return s_count;
}