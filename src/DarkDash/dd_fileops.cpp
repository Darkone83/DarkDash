/*---------------------------------------------------------------------------
    dd_fileops.cpp -- shared file operations + MU management (see dd_fileops.h).

    Operation mechanics ported from XbDiag's FileExplorerOps.cpp (recursive
    copy expansion + recursive delete + 64KB chunked copy) and the MU mount /
    letter-table / format logic from FileExplorerMU.cpp, decoupled from the
    explorer UI so the FTP server and file manager can share them. Kernel
    signatures come from xboxinternals.h (consolidated).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"
#include "dd_fileops.h"
#include "input.h"      /* IsMUPresent */
#include "dd_mount.h"   /* Mount_HddPartitions (single owner of HDD mounting) */

/* ---- small string helpers (no CRT str* per build constraints) ----------- */

static int FoLen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void FoCopy(char* dst, int cap, const char* src) {
    int i = 0;
    if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* append "\name" onto a path buffer (adds the separator if missing) */
static void FoJoin(char* path, int cap, const char* name) {
    int n = FoLen(path), i = 0;
    if (n > 0 && path[n - 1] != '\\' && n < cap - 1) path[n++] = '\\';
    while (name[i] && n < cap - 1) path[n++] = name[i++];
    path[n] = 0;
}

/* ---- existence ----------------------------------------------------------- */

int Fileops_Exists(const char* path) {
    return GetFileAttributesA(path) != 0xFFFFFFFF;
}
int Fileops_IsDir(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return (a != 0xFFFFFFFF) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ---- single file copy (blocking) ---------------------------------------- */

int Fileops_CopyFile(const char* src, const char* dst) {
    /* 64KB copy buffer kept OFF the stack: Fileops_CopyTree recurses, and a
       64KB stack frame per CopyFile on top of nested CopyTree frames overflows
       the Xbox thread stack -> 0x7F double-fault bugcheck. File-scope static is
       safe here (file ops run only on the main thread, never reentrant). */
    static char buf[FILEOPS_COPY_BUF];
    HANDLE hs, hd;
    DWORD  nr, nw;
    int    ok = 1;

    hs = CreateFile(src, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hs == INVALID_HANDLE_VALUE) return 0;

    hd = CreateFile(dst, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hd == INVALID_HANDLE_VALUE) { CloseHandle(hs); return 0; }

    for (;;) {
        if (!ReadFile(hs, buf, sizeof(buf), &nr, NULL) || nr == 0) break;
        if (!WriteFile(hd, buf, nr, &nw, NULL) || nw != nr) { ok = 0; break; }
    }
    CloseHandle(hs);
    FlushFileBuffers(hd);
    CloseHandle(hd);
    return ok;
}

/* ---- recursive copy tree (blocking) ------------------------------------- */

#define FILEOPS_MAX_DEPTH 40   /* deep enough for any real tree, safe on stack */

static int CopyTreeRec(const char* src, const char* dst, int depth) {
    char            pat[FILEOPS_PATH_MAX + 4];
    WIN32_FIND_DATA fd;
    HANDLE          h;
    int             ok = 1, n;

    if (!Fileops_IsDir(src))
        return Fileops_CopyFile(src, dst);

    if (depth >= FILEOPS_MAX_DEPTH) return 0;   /* refuse to recurse deeper */

    CreateDirectoryA(dst, NULL);     /* mkdir target; ignore exists */

    FoCopy(pat, sizeof(pat), src);
    n = FoLen(pat);
    if (n > 0 && pat[n - 1] != '\\' && n < (int)sizeof(pat) - 2) pat[n++] = '\\';
    pat[n++] = '*'; pat[n] = 0;

    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return ok;
    do {
        char s2[FILEOPS_PATH_MAX], d2[FILEOPS_PATH_MAX];
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == 0 || fd.cFileName[1] == '.')) continue;
        FoCopy(s2, sizeof(s2), src); FoJoin(s2, sizeof(s2), fd.cFileName);
        FoCopy(d2, sizeof(d2), dst); FoJoin(d2, sizeof(d2), fd.cFileName);
        if (!CopyTreeRec(s2, d2, depth + 1)) ok = 0;
    } while (FindNextFile(h, &fd));
    FindClose(h);
    return ok;
}

int Fileops_CopyTree(const char* src, const char* dst) {
    return CopyTreeRec(src, dst, 0);
}

/* ---- recursive delete (blocking) ---------------------------------------- */

static int DeleteRec(const char* path, int depth) {
    char            pat[FILEOPS_PATH_MAX + 4];
    WIN32_FIND_DATA fd;
    HANDLE          h;
    int             ok = 1, n;

    if (!Fileops_IsDir(path))
        return DeleteFileA(path) != 0;

    if (depth >= FILEOPS_MAX_DEPTH) return 0;   /* refuse to recurse deeper */

    FoCopy(pat, sizeof(pat), path);
    n = FoLen(pat);
    if (n > 0 && pat[n - 1] != '\\' && n < (int)sizeof(pat) - 2) pat[n++] = '\\';
    pat[n++] = '*'; pat[n] = 0;

    h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char child[FILEOPS_PATH_MAX];
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == 0 || fd.cFileName[1] == '.')) continue;
            FoCopy(child, sizeof(child), path);
            FoJoin(child, sizeof(child), fd.cFileName);
            if (!DeleteRec(child, depth + 1)) ok = 0;
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryA(path)) ok = 0;
    return ok;
}

int Fileops_Delete(const char* path) {
    return DeleteRec(path, 0);
}

/* ---- move = copy then delete source (source kept if copy failed) -------- */

int Fileops_Move(const char* src, const char* dst) {
    /* same-volume rename is the cheap path; MoveFile is unreliable on Xbox FS,
       so only attempt the rename-table move, else fall back to copy+delete. */
    if (Fileops_CopyTree(src, dst)) {
        return Fileops_Delete(src);
    }
    return 0;
}

int Fileops_MkDir(const char* path) {
    if (Fileops_IsDir(path)) return 1;
    return CreateDirectoryA(path, NULL) != 0;
}
int Fileops_RmDir(const char* path) {
    return RemoveDirectoryA(path) != 0;
}

int Fileops_Rename(const char* oldPath, const char* newPath) {
    return MoveFileA(oldPath, newPath) != 0;
}

/* ---- chunked copy -------------------------------------------------------- */

int FileopsCopy_Begin(FileopsCopy* c, const char* src, const char* dst) {
    c->src = c->dst = INVALID_HANDLE_VALUE;
    c->total = c->done = 0; c->active = 0; c->ok = 1;

    c->src = CreateFile(src, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (c->src == INVALID_HANDLE_VALUE) { c->ok = 0; return 0; }

    c->dst = CreateFile(dst, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (c->dst == INVALID_HANDLE_VALUE) {
        CloseHandle(c->src); c->src = INVALID_HANDLE_VALUE; c->ok = 0; return 0;
    }
    c->total = GetFileSize(c->src, NULL);
    c->active = 1;
    return 1;
}

int FileopsCopy_Pump(FileopsCopy* c) {
    DWORD nr = 0, nw = 0;
    if (!c->active) return 0;

    if (!ReadFile(c->src, c->buf, sizeof(c->buf), &nr, NULL) || nr == 0) {
        CloseHandle(c->src); c->src = INVALID_HANDLE_VALUE;
        FlushFileBuffers(c->dst); CloseHandle(c->dst); c->dst = INVALID_HANDLE_VALUE;
        c->active = 0;
        return 0;   /* done */
    }
    if (!WriteFile(c->dst, c->buf, nr, &nw, NULL) || nw != nr) c->ok = 0;
    c->done += nw;
    return 1;       /* more */
}

void FileopsCopy_Abort(FileopsCopy* c) {
    if (c->src != INVALID_HANDLE_VALUE) { CloseHandle(c->src); c->src = INVALID_HANDLE_VALUE; }
    if (c->dst != INVALID_HANDLE_VALUE) { CloseHandle(c->dst); c->dst = INVALID_HANDLE_VALUE; }
    c->active = 0;
}

/* ===========================================================================
   Memory Unit management -- ported from XbDiag FileExplorerMU.cpp.
   Presence comes from input.cpp's XGetDeviceChanges tracking (IsMUPresent).
   =========================================================================== */

   /* MU letter table indexed by port*2+slot (0-7); avoids HDD + DVD letters.
        p0s0=A p0s1=B  p1s0=I p1s1=J  p2s0=K p2s1=L  p3s0=M p3s1=H            */
static const char k_muLetters[8] = { 'A', 'B', 'I', 'J', 'K', 'L', 'M', 'H' };

char Mu_Letter(int port, int slot) {
    int mu = port * 2 + slot;
    if (mu < 0 || mu > 7) return '?';
    return k_muLetters[mu];
}

int Mu_Present(int port, int slot) { return IsMUPresent(port, slot); }

static int MountMus(void) {
    int any = 0, port, slot;
    for (port = 0; port < 4; port++) {
        for (slot = 0; slot < 2; slot++) {
            char   devBuf[64], linkBuf[8];
            STRING devName, sLink;
            if (!IsMUPresent(port, slot)) continue;

            devName.Length = 0; devName.MaximumLength = sizeof(devBuf) - 2;
            devName.Buffer = devBuf;
            if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0) continue;

            linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
            linkBuf[4] = Mu_Letter(port, slot); linkBuf[5] = ':'; linkBuf[6] = 0;
            sLink.Length = 6; sLink.MaximumLength = 7; sLink.Buffer = linkBuf;
            IoCreateSymbolicLink(&sLink, &devName);
            any = 1;
        }
    }
    return any;
}

int Mu_MountAll(void) {
    Mount_HddPartitions();   /* HDD partitions are owned by dd_mount */
    return MountMus();
}

void Mu_Release(int port, int slot) {
    void* devObj = MU_GetExistingDeviceObject((DWORD)port, (DWORD)slot);
    if (devObj) IoDismountVolume((DEVICE_OBJECT*)devObj);
    MU_CloseDeviceObject((DWORD)port, (DWORD)slot);
}

int Mu_Format(int port, int slot) {
    char   devBuf[64], linkBuf[8];
    STRING devName, sLink;
    void* devObj;
    BOOL   ok;

    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = Mu_Letter(port, slot); linkBuf[5] = ':'; linkBuf[6] = 0;
    sLink.Length = 6; sLink.MaximumLength = 7; sLink.Buffer = linkBuf;

    /* 1: dismount existing volume so all handles invalidate */
    devObj = MU_GetExistingDeviceObject((DWORD)port, (DWORD)slot);
    if (devObj) IoDismountVolume((DEVICE_OBJECT*)devObj);
    MU_CloseDeviceObject((DWORD)port, (DWORD)slot);

    /* 2: create device object + format FATX */
    devName.Length = 0; devName.MaximumLength = sizeof(devBuf) - 2; devName.Buffer = devBuf;
    if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0) return 0;

    ok = XapiFormatFATVolumeEx(&devName, 0);
    if (!ok) { MU_CloseDeviceObject((DWORD)port, (DWORD)slot); return 0; }

    /* 3: close + reopen to force a remount on the fresh volume */
    MU_CloseDeviceObject((DWORD)port, (DWORD)slot);
    devName.Length = 0; devName.MaximumLength = sizeof(devBuf) - 2; devName.Buffer = devBuf;
    if (MU_CreateDeviceObject((DWORD)port, (DWORD)slot, &devName) < 0) return 0;

    /* 4: rebind drive letter */
    IoCreateSymbolicLink(&sLink, &devName);
    return 1;
}

int Mu_FreeBytes(const char* driveRoot, unsigned __int64* freeOut,
    unsigned __int64* totalOut) {
    ULARGE_INTEGER freeToCaller, total, freeBytes;
    if (freeOut)  *freeOut = 0;
    if (totalOut) *totalOut = 0;
    if (!GetDiskFreeSpaceExA(driveRoot, &freeToCaller, &total, &freeBytes))
        return 0;
    if (freeOut)  *freeOut = freeToCaller.QuadPart;
    if (totalOut) *totalOut = total.QuadPart;
    return 1;
}