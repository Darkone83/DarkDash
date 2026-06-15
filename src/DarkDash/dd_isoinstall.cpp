/*---------------------------------------------------------------------------
    dd_isoinstall.cpp -- see dd_isoinstall.h.

    Flow (mirrors the Rocky5 XISO-to-HDD installer, native):
      1. validate the XISO
      2. derive "<Name> (ISO)" from the file name
      3. make destRoot\<Name> (ISO)\
      4. extract default.xbe (or game.xbe) from the ISO root
      5. Attach_Build that into attach.xbe, delete the raw xbe, rename
         attach.xbe -> default.xbe
      6. move the .iso in beside it
      7. drop D:\data\default.tbn as a fallback thumbnail (best effort)

    Build: MSVC2003 / C89 style; Win32/Xbox file API. No sprintf/strlen.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <stdlib.h>
#include "dd_isoinstall.h"
#include "dd_xiso.h"
#include "dd_attach.h"
#include "dd_paths.h"
#include "games.h"

#define TBN_TEMPLATE   "D:\\data\\default.tbn"

/* ---- small path helpers (no CRT string funcs) ------------------------- */

static const char* BaseName(const char* path) {
    const char* base = path;
    const char* p = path;
    while (*p) { if (*p == '\\' || *p == '/' || *p == ':') base = p + 1; p++; }
    return base;
}

static void Join(char* out, int cap, const char* a, const char* b) {
    int i = 0, j = 0;
    if (cap <= 0) return;
    while (a[j] && i < cap - 1) out[i++] = a[j++];
    if (i > 0 && out[i - 1] != '\\' && i < cap - 1) out[i++] = '\\';
    j = 0;
    while (b[j] && i < cap - 1) out[i++] = b[j++];
    out[i] = 0;
}

/* case-insensitive trailing-suffix strip; returns the new length */
static int StripSuffixI(char* s, int n, const char* suf) {
    int sl = 0, i;
    while (suf[sl]) sl++;
    if (n < sl) return n;
    for (i = 0; i < sl; i++) {
        char a = s[n - sl + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return n;
    }
    n -= sl;
    s[n] = 0;
    return n;
}

/* destination image name: original stem with a normalized .iso extension, so
   Cerbios mounts it regardless of whether the source was .iso/.xiso/.xiso.iso */
static void BuildImageName(const char* isoPath, char* out, int cap) {
    const char* b = BaseName(isoPath);
    const char* ext = ".iso";
    int n = 0, i;
    while (b[n] && n < cap - 6) { out[n] = b[n]; n++; }
    out[n] = 0;
    n = StripSuffixI(out, n, ".iso");
    n = StripSuffixI(out, n, ".xiso");
    for (i = 0; ext[i] && n < cap - 1; i++) out[n++] = ext[i];
    out[n] = 0;
}

/* "Halo 2 (USA)_1.iso" -> "Halo 2 (ISO)" : drop ext, split-marker, region tag */
static void DeriveFolderName(const char* isoPath, char* out, int cap) {
    char name[128];
    const char* base = BaseName(isoPath);
    int n = 0, i;
    static const char* suf = " (ISO)";

    while (base[n] && n < 127) { name[n] = base[n]; n++; }
    name[n] = 0;

    /* strip a trailing .iso, then a trailing .xiso (handles "name.xiso.iso") */
    n = StripSuffixI(name, n, ".iso");
    n = StripSuffixI(name, n, ".xiso");
    /* strip a trailing split marker _1/_2/.1/.2 */
    if (n >= 2) {
        char c0 = name[n - 2], c1 = name[n - 1];
        if ((c0 == '_' || c0 == '.') && (c1 == '1' || c1 == '2')) { name[n - 2] = 0; n -= 2; }
    }
    /* keep only the text before the first '(' (region / version tags) */
    for (i = 0; i < n; i++) { if (name[i] == '(') { name[i] = 0; n = i; break; } }
    /* trim trailing spaces */
    while (n > 0 && name[n - 1] == ' ') { name[--n] = 0; }
    /* cap the stem length, then re-trim */
    if (n > 36) { name[36] = 0; n = 36; while (n > 0 && name[n - 1] == ' ') name[--n] = 0; }
    if (n == 0) { name[0] = 'G'; name[1] = 'a'; name[2] = 'm'; name[3] = 'e'; name[4] = 0; n = 4; }

    /* out = name + " (ISO)" */
    {
        int o = 0; i = 0;
        while (name[i] && o < cap - 1) out[o++] = name[i++];
        i = 0;
        while (suf[i] && o < cap - 1) out[o++] = suf[i++];
        out[o] = 0;
    }
}

/* ---- file helpers ----------------------------------------------------- */

int IsoInstall_RootAccessible(const char* root) {
    if (!root || !root[0]) return 0;
    if (CreateDirectoryA(root, NULL)) return 1;           /* created it       */
    return (GetLastError() == ERROR_ALREADY_EXISTS);      /* drive is mounted */
}

static int CopyBuffered(const char* src, const char* dst) {
    HANDLE in, out; BYTE* buf; DWORD got, wrote; int ok = 1;
    in = CreateFileA(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    out = CreateFileA(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return 0; }
    buf = (BYTE*)malloc(65536);
    if (!buf) { CloseHandle(in); CloseHandle(out); return 0; }
    for (;;) {
        got = 0;
        if (!ReadFile(in, buf, 65536, &got, NULL)) { ok = 0; break; }
        if (got == 0) break;
        wrote = 0;
        if (!WriteFile(out, buf, got, &wrote, NULL) || wrote != got) { ok = 0; break; }
    }
    free(buf);
    CloseHandle(in);
    CloseHandle(out);
    return ok;
}

/* MoveFile, falling back to copy+delete across volumes (large .iso copies). */
static int MoveSmart(const char* src, const char* dst) {
    DeleteFileA(dst);                       /* allow re-install over an old copy */
    if (MoveFileA(src, dst)) return 1;
    if (CopyBuffered(src, dst)) { DeleteFileA(src); return 1; }
    return 0;
}

/* ---- public ----------------------------------------------------------- */

int IsoInstall_DefaultRoot(char* out, int cap) {
    const LauncherConfig* cfg;
    int np, k, i;
    if (!out || cap <= 0) return 0;
    out[0] = 0;

    np = Paths_Count("games");
    for (k = 0; k < np; k++) {
        const char* p = Paths_Get("games", k);
        if (IsoInstall_RootAccessible(p)) { Join(out, cap, p, ""); return 1; }
    }
    cfg = Games_Config();
    if (cfg && cfg->roots) {
        for (i = 0; i < cfg->rootCount; i++) {
            if (IsoInstall_RootAccessible(cfg->roots[i])) { Join(out, cap, cfg->roots[i], ""); return 1; }
        }
    }
    return 0;
}

int IsoInstall_Run(const char* isoPath, const char* destRoot) {
    char folder[260], xbePath[260], attachPath[260], dstPath[260];

    if (!isoPath || !destRoot) return ISOINST_ERR_ARG;
    if (!Xiso_IsValid(isoPath)) return ISOINST_ERR_NOTXISO;

    /* destRoot\<Name> (ISO)\ */
    {
        char fname[64];
        DeriveFolderName(isoPath, fname, sizeof(fname));
        Join(folder, sizeof(folder), destRoot, fname);
    }
    if (!CreateDirectoryA(folder, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return ISOINST_ERR_MKDIR;

    /* extract the boot xbe as default.xbe (try game.xbe if absent) */
    Join(xbePath, sizeof(xbePath), folder, "default.xbe");
    if (!Xiso_ExtractRoot(isoPath, "default.xbe", xbePath)) {
        if (!Xiso_ExtractRoot(isoPath, "game.xbe", xbePath)) {
            RemoveDirectoryA(folder);
            return ISOINST_ERR_NOXBE;
        }
    }

    /* stamp it into an attach stub, then swap attach.xbe -> default.xbe */
    Join(attachPath, sizeof(attachPath), folder, "attach.xbe");
    if (!Attach_Build(xbePath, attachPath)) {
        DeleteFileA(xbePath);
        RemoveDirectoryA(folder);
        return ISOINST_ERR_ATTACH;
    }
    DeleteFileA(xbePath);
    if (!MoveFileA(attachPath, xbePath)) {       /* same dir -> instant rename */
        DeleteFileA(attachPath);
        RemoveDirectoryA(folder);
        return ISOINST_ERR_ATTACH;
    }

    /* move the .iso in beside the stub, normalizing its name to <stem>.iso */
    {
        char imgName[128];
        BuildImageName(isoPath, imgName, sizeof(imgName));
        Join(dstPath, sizeof(dstPath), folder, imgName);
    }
    if (!MoveSmart(isoPath, dstPath)) {
        /* stub is built but the image isn't placed: leave the folder for retry */
        return ISOINST_ERR_MOVE;
    }

    /* fallback thumbnail (best effort -- the embedded icon is the primary) */
    Join(dstPath, sizeof(dstPath), folder, "default.tbn");
    CopyBuffered(TBN_TEMPLATE, dstPath);

    return ISOINST_OK;
}

int IsoInstall_RunDefault(const char* isoPath) {
    char root[260];
    if (!IsoInstall_DefaultRoot(root, sizeof(root))) return ISOINST_ERR_NODEST;
    return IsoInstall_Run(isoPath, root);
}