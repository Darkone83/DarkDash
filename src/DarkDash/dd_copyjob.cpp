/*---------------------------------------------------------------------------
    dd_copyjob.cpp -- see dd_copyjob.h.

    Streaming tree walk with an explicit handle stack (bounded by depth) feeding
    the existing pumpable FileopsCopy for per-file chunked transfer. One file's
    worth of chunks is moved per Pump tick group; the FileMan loop pumps a few
    times per frame for throughput while staying responsive.

    C89 style: declarations before statements, file-scope statics, no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_copyjob.h"
#include "dd_fileops.h"

#define CJ_MAX_ITEMS  512        /* top-level selections */
#define CJ_MAX_DEPTH  40         /* matches Fileops recursion guard */
#define CJ_CHUNKS_PER_PUMP 2     /* FileopsCopy pumps per CopyJob_Pump call */

/* a directory-walk frame: an open find handle + the src/dst dirs it lists */
typedef struct {
    HANDLE          h;
    WIN32_FIND_DATA  fd;
    int             first;                   /* 1 until we consume fd from FindFirstFile */
    char            src[COPYJOB_PATH_MAX];
    char            dst[COPYJOB_PATH_MAX];
} CjFrame;

static int          s_active = 0;
static int          s_state = CJ_IDLE;
static int          s_isMove = 0;
static int          s_cancel = 0;

static CopyJobItem  s_items[CJ_MAX_ITEMS];
static int          s_nItems = 0;
static int          s_itemIdx = 0;           /* next top-level item to start */
static char         s_destDir[COPYJOB_PATH_MAX];

static CjFrame      s_stk[CJ_MAX_DEPTH];
static int          s_depth = 0;             /* number of active frames */

static FileopsCopy  s_copy;                  /* in-flight file copy */
static int          s_copying = 0;
static char         s_curSrc[COPYJOB_PATH_MAX];  /* source of in-flight file (for move-delete) */
static char         s_curName[COPYJOB_NAME_MAX];

static int          s_filesDone = 0;
static int          s_filesSeen = 0;

/* ---- string helpers ---------------------------------------------------- */

static int CjLen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static void CjCopy(char* dst, int cap, const char* src) {
    int i = 0; if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void CjJoin(char* out, int cap, const char* base, const char* leaf) {
    int n;
    CjCopy(out, cap, base);
    n = CjLen(out);
    if (n > 0 && out[n - 1] != '\\' && n < cap - 1) { out[n++] = '\\'; out[n] = 0; }
    { int i = 0; while (leaf && leaf[i] && n < cap - 1) out[n++] = leaf[i++]; out[n] = 0; }
}

static int CjIsDotDir(const char* n) {
    return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

/* ---- walk-stack ops ---------------------------------------------------- */

static void CjPushDir(const char* src, const char* dst) {
    CjFrame* f;
    char pat[COPYJOB_PATH_MAX + 4];
    int n;
    if (s_depth >= CJ_MAX_DEPTH) return;     /* refuse deeper -> tree truncated */
    CreateDirectoryA(dst, NULL);             /* ensure target dir exists */

    f = &s_stk[s_depth];
    CjCopy(f->src, sizeof(f->src), src);
    CjCopy(f->dst, sizeof(f->dst), dst);
    CjCopy(pat, sizeof(pat), src);
    n = CjLen(pat);
    if (n > 0 && pat[n - 1] != '\\' && n < (int)sizeof(pat) - 2) pat[n++] = '\\';
    pat[n++] = '*'; pat[n] = 0;

    f->h = FindFirstFileA(pat, &f->fd);
    f->first = 1;
    s_depth++;
}

static void CjPopDir(void) {
    if (s_depth <= 0) return;
    s_depth--;
    if (s_stk[s_depth].h != INVALID_HANDLE_VALUE) FindClose(s_stk[s_depth].h);
    s_stk[s_depth].h = INVALID_HANDLE_VALUE;
}

static void CjCloseAll(void) {
    while (s_depth > 0) CjPopDir();
    if (s_copying) { FileopsCopy_Abort(&s_copy); s_copying = 0; }
}

/* ---- public ------------------------------------------------------------ */

int CopyJob_Begin(const CopyJobItem* items, int nItems,
    const char* destDir, int isMove) {
    int i;
    if (!items || nItems <= 0 || !destDir || !destDir[0]) return 0;
    if (nItems > CJ_MAX_ITEMS) nItems = CJ_MAX_ITEMS;

    for (i = 0; i < nItems; i++) s_items[i] = items[i];
    s_nItems = nItems;
    CjCopy(s_destDir, sizeof(s_destDir), destDir);

    s_isMove = isMove ? 1 : 0;
    s_cancel = 0;
    s_itemIdx = 0;
    s_depth = 0;
    s_copying = 0;
    s_filesDone = 0;
    s_filesSeen = 0;
    s_curName[0] = 0;
    s_curSrc[0] = 0;
    s_active = 1;
    s_state = CJ_RUNNING;
    return 1;
}

/* start copying one file; returns 1 if a copy began */
static int CjBeginFile(const char* src, const char* dst, const char* name) {
    if (!FileopsCopy_Begin(&s_copy, src, dst)) return 0;
    CjCopy(s_curSrc, sizeof(s_curSrc), src);
    CjCopy(s_curName, sizeof(s_curName), name);
    s_copying = 1;
    s_filesSeen++;
    return 1;
}

/* finish the in-flight file: close, (move) delete source, count it */
static void CjEndFile(void) {
    int okFile = s_copy.ok;
    s_copying = 0;
    if (okFile && s_isMove && s_curSrc[0]) DeleteFileA(s_curSrc);
    if (okFile) s_filesDone++;
    else        s_state = CJ_FAILED;   /* a file failed; flag but keep unwinding */
}

/* advance the directory walk by one entry; may push a dir or start a file.
   Returns 1 if it did something, 0 if the whole job's walk is exhausted. */
static int CjStepWalk(void) {
    /* seed next top-level item if the stack is empty */
    if (s_depth == 0) {
        char dst[COPYJOB_PATH_MAX];
        const CopyJobItem* it;
        if (s_itemIdx >= s_nItems) return 0;     /* nothing left */
        it = &s_items[s_itemIdx++];
        CjJoin(dst, sizeof(dst), s_destDir, it->name);
        if (Fileops_IsDir(it->src)) {
            CjPushDir(it->src, dst);
        }
        else {
            if (CjBeginFile(it->src, dst, it->name)) return 1;
            /* failed to open -> mark and continue */
            s_state = CJ_FAILED;
        }
        return 1;
    }

    {
        CjFrame* f = &s_stk[s_depth - 1];
        BOOL have;
        if (f->h == INVALID_HANDLE_VALUE) { CjPopDir(); return 1; }
        if (f->first) { f->first = 0; have = TRUE; }
        else { have = FindNextFileA(f->h, &f->fd); }
        if (!have) { CjPopDir(); return 1; }
        if (CjIsDotDir(f->fd.cFileName)) return 1;

        {
            char cs[COPYJOB_PATH_MAX], cd[COPYJOB_PATH_MAX];
            CjJoin(cs, sizeof(cs), f->src, f->fd.cFileName);
            CjJoin(cd, sizeof(cd), f->dst, f->fd.cFileName);
            if (f->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                CjPushDir(cs, cd);
            }
            else {
                if (!CjBeginFile(cs, cd, f->fd.cFileName)) s_state = CJ_FAILED;
            }
        }
        return 1;
    }
}

int CopyJob_Pump(void) {
    int i;
    if (!s_active) return CJ_IDLE;

    if (s_cancel) {
        CjCloseAll();
        s_active = 0;
        s_state = CJ_CANCELLED;
        return CJ_CANCELLED;
    }

    /* do a little work: pump the in-flight file a few chunks, else walk. */
    for (i = 0; i < CJ_CHUNKS_PER_PUMP; i++) {
        if (s_copying) {
            if (!FileopsCopy_Pump(&s_copy)) {   /* file finished */
                CjEndFile();
            }
        }
        else {
            if (!CjStepWalk()) {
                /* walk exhausted and nothing copying -> we're done.
                   For a move, remove now-empty source directories. */
                if (s_state == CJ_RUNNING) {
                    if (s_isMove) {
                        int k;
                        for (k = 0; k < s_nItems; k++)
                            if (Fileops_IsDir(s_items[k].src))
                                Fileops_Delete(s_items[k].src);  /* prune emptied tree */
                    }
                    s_state = CJ_DONE;
                }
                s_active = 0;
                return s_state;     /* DONE or FAILED */
            }
        }
    }
    return CJ_RUNNING;
}

void CopyJob_Cancel(void) {
    if (s_active) s_cancel = 1;
}

int CopyJob_Active(void) {
    return s_active;
}

void CopyJob_Progress(int* filesDone, int* filesSeen,
    char* curName, int cap,
    unsigned* curDone, unsigned* curTotal) {
    if (filesDone) *filesDone = s_filesDone;
    if (filesSeen) *filesSeen = s_filesSeen;
    if (curName && cap > 0) CjCopy(curName, cap, s_curName);
    if (curDone)  *curDone = (unsigned)s_copy.done;
    if (curTotal) *curTotal = (unsigned)s_copy.total;
}