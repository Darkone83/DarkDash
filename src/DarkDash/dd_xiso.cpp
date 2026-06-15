/*---------------------------------------------------------------------------
    dd_xiso.cpp -- see dd_xiso.h.

    XDVDFS layout (the parts we need):
      0x10000  : 20-byte signature "MICROSOFT*XBOX*MEDIA"
      0x10014  : root directory sector (u32 LE)   [2048-byte sectors]
      0x10018  : root directory size  (u32 LE, bytes)

    Directory entry (within the root dir table, 4-byte aligned):
      +0  u16 left  subtree offset (in 4-byte units; 0 / 0xFFFF = none)
      +2  u16 right subtree offset
      +4  u32 start sector
      +8  u32 file size
      +12 u8  attributes
      +13 u8  name length
      +14 ..  name (ASCII)

    We visit every node from the root (it is a BST, so all entries are reachable
    via left/right) and compare names case-insensitively -- this avoids needing
    to reproduce the exact XDVDFS collation a binary search would require.

    Build: MSVC2003 / C89 style; Win32/Xbox file API; 64-bit seeks (a dual-layer
    XISO can place files past the 4 GB mark).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <stdlib.h>
#include "dd_xiso.h"

#define XISO_SIG_OFF   0x10000
#define XISO_SECTOR    0x800
#define XISO_DIR_CAP   (256u * 1024u)   /* sanity cap on a root dir table */

static const unsigned char XISO_SIG[20] = {
    'M','I','C','R','O','S','O','F','T','*','X','B','O','X','*','M','E','D','I','A'
};

static unsigned Rd16(const unsigned char* p) { return (unsigned)(p[0] | (p[1] << 8)); }
static unsigned Rd32(const unsigned char* p) {
    return (unsigned)(p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24));
}

static int SeekTo(HANDLE h, LONGLONG off) {
    LONG hi = (LONG)(off >> 32);
    DWORD r = SetFilePointer(h, (LONG)(off & 0xFFFFFFFF), &hi, FILE_BEGIN);
    return !(r == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR);
}

static int ReadAt(HANDLE h, LONGLONG off, void* buf, DWORD len) {
    DWORD got = 0;
    if (!SeekTo(h, off)) return 0;
    if (!ReadFile(h, buf, len, &got, NULL) || got != len) return 0;
    return 1;
}

static char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* entry-name (alen bytes, not NUL-terminated) vs C-string `b`, case-insensitive */
static int NameEqI(const char* a, int alen, const char* b) {
    int i;
    for (i = 0; i < alen; i++) {
        if (b[i] == 0) return 0;
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return 0;
    }
    return b[alen] == 0;
}

int Xiso_IsValid(const char* isoPath) {
    HANDLE h; unsigned char sig[20]; int ok, i;
    if (!isoPath) return 0;
    h = CreateFileA(isoPath, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = ReadAt(h, XISO_SIG_OFF, sig, 20);
    CloseHandle(h);
    if (!ok) return 0;
    for (i = 0; i < 20; i++) if (sig[i] != XISO_SIG[i]) return 0;
    return 1;
}

static int ReadRootHdr(HANDLE h, unsigned* sector, unsigned* size) {
    unsigned char hdr[28];   /* 20 sig + 4 sector + 4 size */
    int i;
    if (!ReadAt(h, XISO_SIG_OFF, hdr, 28)) return 0;
    for (i = 0; i < 20; i++) if (hdr[i] != XISO_SIG[i]) return 0;
    *sector = Rd32(hdr + 20);
    *size = Rd32(hdr + 24);
    return 1;
}

int Xiso_FindRoot(const char* isoPath, const char* name,
    unsigned* sectorOut, unsigned* sizeOut) {
    HANDLE h; unsigned rootSec, rootSize; unsigned char* tbl;
    unsigned stack[256]; int sp = 0, guard = 0, found = 0;
    if (!isoPath || !name) return 0;
    h = CreateFileA(isoPath, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadRootHdr(h, &rootSec, &rootSize) || rootSize == 0 || rootSize > XISO_DIR_CAP) {
        CloseHandle(h); return 0;
    }
    tbl = (unsigned char*)malloc(rootSize);
    if (!tbl) { CloseHandle(h); return 0; }
    if (!ReadAt(h, (LONGLONG)rootSec * XISO_SECTOR, tbl, rootSize)) {
        free(tbl); CloseHandle(h); return 0;
    }
    CloseHandle(h);

    stack[sp++] = 0;   /* root node at table offset 0 */
    while (sp > 0 && guard++ < 8192) {
        unsigned nodeOff = stack[--sp];
        unsigned byteOff = nodeOff * 4;
        unsigned left, right, sec, sz, nl;
        const char* nm;
        if (byteOff + 14 > rootSize) continue;
        left = Rd16(tbl + byteOff + 0);
        right = Rd16(tbl + byteOff + 2);
        sec = Rd32(tbl + byteOff + 4);
        sz = Rd32(tbl + byteOff + 8);
        nl = tbl[byteOff + 13];
        nm = (const char*)(tbl + byteOff + 14);
        if (byteOff + 14 + nl <= rootSize && NameEqI(nm, (int)nl, name)) {
            if (sectorOut) *sectorOut = sec;
            if (sizeOut)   *sizeOut = sz;
            found = 1;
            break;
        }
        if (left && left != 0xFFFF && sp < 256) stack[sp++] = left;
        if (right && right != 0xFFFF && sp < 256) stack[sp++] = right;
    }
    free(tbl);
    return found;
}

int Xiso_ExtractRoot(const char* isoPath, const char* name, const char* destPath) {
    HANDLE in, out; unsigned sec, sz; LONGLONG off;
    unsigned char* buf; DWORD remain; int ok = 1;
    if (!Xiso_FindRoot(isoPath, name, &sec, &sz)) return 0;
    in = CreateFileA(isoPath, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in == INVALID_HANDLE_VALUE) return 0;
    out = CreateFileA(destPath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return 0; }

    off = (LONGLONG)sec * XISO_SECTOR;
    if (!SeekTo(in, off)) ok = 0;
    buf = ok ? (unsigned char*)malloc(65536) : NULL;
    if (!buf) ok = 0;
    remain = sz;
    while (ok && remain > 0) {
        DWORD chunk = (remain < 65536) ? remain : 65536;
        DWORD got = 0, wrote = 0;
        if (!ReadFile(in, buf, chunk, &got, NULL) || got == 0) { ok = 0; break; }  /* past EOF = split image */
        if (!WriteFile(out, buf, got, &wrote, NULL) || wrote != got) { ok = 0; break; }
        remain -= got;
    }
    if (buf) free(buf);
    CloseHandle(in);
    CloseHandle(out);
    return (ok && remain == 0);
}