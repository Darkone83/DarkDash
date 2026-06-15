/*---------------------------------------------------------------------------
    dd_attach.cpp -- see dd_attach.h.

    Template offsets (verified against the shipped attach.xbe, base 0x10000):
      0x10C  base file size              (u32)
      0x118  cert VA  -> cert file off 0x184
      0x184  certificate (464 bytes)     <- copied from the game
      0x184+0xAC (0x230) dwVersion       <- stamped 0x80000001
      0x420  XPR0 virtual size           (u32) <- set to image size
      0x424  XPR0 image file offset      (u32) = 0x11000 (where image goes)
      0x428  XPR0 raw size               (u32) <- set to image size

    Build: MSVC2003 / C89 style; Win32/Xbox file API.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <stdlib.h>
#include <string.h>
#include "dd_attach.h"
#include "dd_xbe.h"

#define ATTACH_TEMPLATE     "D:\\data\\attach.xbe"
#define CERT_FILE_OFF       0x184
#define CERT_SIZE           464          /* 0x1D0 */
#define VER_OFF             (CERT_FILE_OFF + 0xAC)   /* dwVersion within the cert */
#define OFF_BASE_FILESIZE   0x10C
#define OFF_XPR_VSIZE       0x420
#define OFF_XPR_VADDR       0x424
#define OFF_XPR_RSIZE       0x428
#define XPR_MAX             (256u * 1024u)
#define TPL_MIN             0x12000u      /* must hold header + the XPR region start */

static DWORD Rd32a(const BYTE* p) {
    return (DWORD)(p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24));
}
static void Wr32a(BYTE* p, DWORD v) {
    p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24);
}

static int ReadWhole(const char* path, BYTE** outBuf, DWORD* outSize) {
    HANDLE h; DWORD sz, got = 0; BYTE* b;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0) { CloseHandle(h); return 0; }
    b = (BYTE*)malloc(sz);
    if (!b) { CloseHandle(h); return 0; }
    if (!ReadFile(h, b, sz, &got, NULL) || got != sz) { free(b); CloseHandle(h); return 0; }
    CloseHandle(h);
    *outBuf = b; *outSize = sz;
    return 1;
}

/* Read the game's 464-byte certificate into certOut. Returns 1 on success. */
static int ReadGameCert(const char* gameXbePath, BYTE* certOut) {
    HANDLE g; BYTE hd[0x200]; DWORD got = 0, base, certVA, certOff;
    g = CreateFileA(gameXbePath, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(g, hd, sizeof(hd), &got, NULL) || got < 0x11C) { CloseHandle(g); return 0; }
    if (hd[0] != 'X' || hd[1] != 'B' || hd[2] != 'E' || hd[3] != 'H') { CloseHandle(g); return 0; }
    base = Rd32a(hd + 0x104);
    certVA = Rd32a(hd + 0x118);
    if (certVA < base) { CloseHandle(g); return 0; }
    certOff = certVA - base;
    SetFilePointer(g, certOff, NULL, FILE_BEGIN);
    if (!ReadFile(g, certOut, CERT_SIZE, &got, NULL) || got != CERT_SIZE) { CloseHandle(g); return 0; }
    CloseHandle(g);
    return 1;
}

int Attach_Build(const char* gameXbePath, const char* outAttachPath) {
    BYTE* tpl = NULL; DWORD tplSize = 0;
    BYTE  gameCert[CERT_SIZE];
    BYTE* xpr = NULL; DWORD xprSize = 0;
    BYTE* outBuf = NULL; DWORD outSize = 0;
    HANDLE h; DWORD wrote = 0; int ok = 0;

    if (!gameXbePath || !outAttachPath) return 0;

    /* 1. template */
    if (!ReadWhole(ATTACH_TEMPLATE, &tpl, &tplSize)) return 0;
    if (tplSize < TPL_MIN) { free(tpl); return 0; }

    /* 2. game certificate */
    if (!ReadGameCert(gameXbePath, gameCert)) { free(tpl); return 0; }

    /* 3. stamp cert + version flag into the template */
    memcpy(tpl + CERT_FILE_OFF, gameCert, CERT_SIZE);
    tpl[VER_OFF + 0] = 0x01; tpl[VER_OFF + 1] = 0x00;
    tpl[VER_OFF + 2] = 0x00; tpl[VER_OFF + 3] = 0x80;   /* dwVersion = 0x80000001 */

    /* 4. embed the game's title image (skip cleanly if absent or oversize) */
    xpr = (BYTE*)malloc(XPR_MAX);
    if (xpr && Xbe_ExtractTitleXpr0(gameXbePath, xpr, XPR_MAX, &xprSize) && xprSize >= 0x20) {
        DWORD imgAddr = Rd32a(tpl + OFF_XPR_VADDR);     /* file offset for the image (0x11000) */
        if (imgAddr >= 0x1000 && imgAddr <= tplSize) {
            outSize = imgAddr + xprSize;
            if (outSize < tplSize) outSize = tplSize;   /* keep template tail if image is smaller */
            outBuf = (BYTE*)malloc(outSize);
            if (outBuf) {
                memset(outBuf, 0, outSize);
                memcpy(outBuf, tpl, tplSize);
                memcpy(outBuf + imgAddr, xpr, xprSize);
                Wr32a(outBuf + OFF_XPR_VSIZE, xprSize);
                Wr32a(outBuf + OFF_XPR_RSIZE, xprSize);
                Wr32a(outBuf + OFF_BASE_FILESIZE, imgAddr + xprSize);
            }
        }
    }

    /* no image (or alloc failed): ship the cert-stamped template as-is */
    if (!outBuf) {
        outBuf = tpl;   /* reuse the template buffer as the output */
        outSize = tplSize;
        tpl = NULL;     /* avoid a double free below */
    }

    /* 5. write the attach.xbe */
    h = CreateFileA(outAttachPath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (WriteFile(h, outBuf, outSize, &wrote, NULL) && wrote == outSize) ok = 1;
        CloseHandle(h);
    }

    if (xpr)    free(xpr);
    if (tpl)    free(tpl);
    if (outBuf) free(outBuf);
    return ok;
}