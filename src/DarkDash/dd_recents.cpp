/*---------------------------------------------------------------------------
    dd_recents.cpp -- see dd_recents.h.

    Persists a small most-recently-used title list to D:\data\recents.dat as a
    versioned fixed-size blob (so a partial/old file is rejected cleanly). C89
    style: declarations before statements, file-scope statics, no sprintf/CRT
    string funcs beyond memcpy/memset.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_recents.h"

#define RECENTS_DIR   "D:\\data"
#define RECENTS_FILE  "D:\\data\\recents.dat"
#define RECENTS_MAGIC 0x31435252u   /* 'RRC1' */

typedef struct {
    char name[RECENTS_NAME_MAX];
    char path[RECENTS_PATH_MAX];
} RecentEntry;

typedef struct {
    unsigned    magic;
    int         count;
    RecentEntry e[RECENTS_MAX];
} RecentsBlob;

static RecentsBlob s_rc;

/* ---- tiny helpers (no CRT str*) ---------------------------------------- */

static void RcCopy(char* dst, int cap, const char* src) {
    int i = 0; if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int RcEqual(const char* a, const char* b) {
    int i = 0;
    for (;;) {
        char ca = a ? a[i] : 0, cb = b ? b[i] : 0;
        /* case-insensitive: XBE paths are not case-sensitive on FATX */
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        if (ca == 0) return 1;
        i++;
    }
}

/* ---- persistence ------------------------------------------------------- */

static void RcSave(void) {
    HANDLE h;
    DWORD  wrote = 0;
    s_rc.magic = RECENTS_MAGIC;
    CreateDirectoryA(RECENTS_DIR, NULL);            /* ensure D:\data exists */
    h = CreateFileA(RECENTS_FILE, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;          /* read-only volume: ignore */
    WriteFile(h, &s_rc, sizeof(s_rc), &wrote, NULL);
    CloseHandle(h);
}

void Recents_Init(void) {
    HANDLE h;
    DWORD  got = 0;
    RecentsBlob tmp;

    s_rc.magic = RECENTS_MAGIC;
    s_rc.count = 0;

    h = CreateFileA(RECENTS_FILE, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (ReadFile(h, &tmp, sizeof(tmp), &got, NULL) && got == sizeof(tmp) &&
        tmp.magic == RECENTS_MAGIC && tmp.count >= 0 && tmp.count <= RECENTS_MAX) {
        s_rc = tmp;
    }
    CloseHandle(h);
}

/* ---- list ops ---------------------------------------------------------- */

void Recents_Add(const char* label, const char* xbePath) {
    int i, found = -1;
    RecentEntry moved;

    if (!xbePath || !xbePath[0]) return;

    /* find an existing entry with the same path */
    for (i = 0; i < s_rc.count; i++) {
        if (RcEqual(s_rc.e[i].path, xbePath)) { found = i; break; }
    }

    if (found >= 0) {
        /* pull it out, shift the rest down, it goes back to front */
        moved = s_rc.e[found];
        for (i = found; i > 0; i--) s_rc.e[i] = s_rc.e[i - 1];
        s_rc.e[0] = moved;
        /* refresh the label in case it changed */
        RcCopy(s_rc.e[0].name, RECENTS_NAME_MAX, (label && label[0]) ? label : s_rc.e[0].name);
    }
    else {
        int n = s_rc.count;
        if (n > RECENTS_MAX - 1) n = RECENTS_MAX - 1;   /* drop the oldest */
        for (i = n; i > 0; i--) s_rc.e[i] = s_rc.e[i - 1];
        RcCopy(s_rc.e[0].name, RECENTS_NAME_MAX, (label && label[0]) ? label : xbePath);
        RcCopy(s_rc.e[0].path, RECENTS_PATH_MAX, xbePath);
        s_rc.count = (s_rc.count < RECENTS_MAX) ? s_rc.count + 1 : RECENTS_MAX;
    }
    RcSave();
}

int Recents_Count(void) {
    return (s_rc.count < 0) ? 0 : (s_rc.count > RECENTS_MAX ? RECENTS_MAX : s_rc.count);
}

const char* Recents_Name(int i) {
    if (i < 0 || i >= Recents_Count()) return "";
    return s_rc.e[i].name;
}

const char* Recents_Path(int i) {
    if (i < 0 || i >= Recents_Count()) return "";
    return s_rc.e[i].path;
}