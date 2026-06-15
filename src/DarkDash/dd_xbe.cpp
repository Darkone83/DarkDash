/*---------------------------------------------------------------------------
    dd_xbe.cpp -- XBE certificate reader.

    Reads only the first 8 KB of the file (the header region always contains
    the certificate -- certOff is tiny), so labelling a list of apps never
    pulls a whole multi-MB executable off disk. Byte-wise field reads keep it
    alignment-safe.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <string.h>
#include "dd_texture.h"
#include "dd_xbe.h"

#define XBE_HDR_READ 8192

static DWORD Rd32(const BYTE* p) {
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

int Xbe_ReadTitle(const char* xbePath, char* nameOut, int nameCap, unsigned* titleIdOut) {
    HANDLE h;
    BYTE   buf[XBE_HDR_READ];
    DWORD  got = 0;
    DWORD  base, certVA, certOff;
    int    i, n;

    if (nameOut && nameCap > 0) nameOut[0] = 0;
    if (titleIdOut) *titleIdOut = 0;

    h = CreateFile(xbePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, buf, XBE_HDR_READ, &got, NULL)) { CloseHandle(h); return 0; }
    CloseHandle(h);

    if (got < 0x120) return 0;
    if (buf[0] != 'X' || buf[1] != 'B' || buf[2] != 'E' || buf[3] != 'H') return 0;

    base = Rd32(buf + 0x104);
    certVA = Rd32(buf + 0x118);
    if (certVA < base) return 0;
    certOff = certVA - base;
    if (certOff + 0xB0 > got) return 0;           /* cert beyond what we read */

    if (titleIdOut) *titleIdOut = Rd32(buf + certOff + 0x08);

    if (nameOut && nameCap > 0) {
        const BYTE* w = buf + certOff + 0x0C;     /* 40 UTF-16LE code units */
        n = 0;
        for (i = 0; i < 40 && n < nameCap - 1; i++) {
            unsigned ch = (unsigned)w[i * 2] | ((unsigned)w[i * 2 + 1] << 8);
            if (ch == 0) break;
            if (ch >= 32 && ch < 127) nameOut[n++] = (char)ch;   /* printable */
            else if (ch >= 127)       nameOut[n++] = '?';        /* non-ASCII */
            /* control chars dropped */
        }
        while (n > 0 && nameOut[n - 1] == ' ') n--;   /* trim trailing pad */
        nameOut[n] = 0;
    }
    return 1;
}

/* ---- title image ($$XTIMAGE, XPR0) ------------------------------------- */

#define XBE_SECHDR_READ 16384   /* header region holds the section table   */

int Xbe_LoadTitleImage(IDirect3DDevice8* dev, const char* xbePath, Texture* out) {
    HANDLE h;
    BYTE   hbuf[XBE_SECHDR_READ];
    BYTE   xpr[0x20];
    DWORD  got = 0, rd = 0;
    DWORD  base, nsec, secVA, secOff;
    DWORD  imgRaw = 0, imgSize = 0;
    DWORD  total, hsize, fmt, fcode, w, hh, pixBytes, dataOff;
    D3DFORMAT      d3dfmt;
    IDirect3DTexture8* tex = NULL;
    D3DLOCKED_RECT lr;
    DWORD  i;

    if (out) { out->tex = NULL; out->w = out->h = out->pw = out->ph = 0; }
    if (!dev || !out) return 0;

    h = CreateFile(xbePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { return 0; }
    if (!ReadFile(h, hbuf, XBE_SECHDR_READ, &got, NULL) || got < 0x140) { CloseHandle(h); return 0; }
    if (hbuf[0] != 'X' || hbuf[1] != 'B' || hbuf[2] != 'E' || hbuf[3] != 'H') { CloseHandle(h); return 0; }

    base = Rd32(hbuf + 0x104);
    nsec = Rd32(hbuf + 0x11C);
    secVA = Rd32(hbuf + 0x120);
    if (secVA < base || nsec == 0 || nsec > 64) { CloseHandle(h); return 0; }
    secOff = secVA - base;

    /* find the $$XTIMAGE section header */
    for (i = 0; i < nsec; i++) {
        DWORD s = secOff + i * 0x38;
        DWORD nameVA, nameOff;
        if (s + 0x38 > got) break;
        nameVA = Rd32(hbuf + s + 0x14);
        if (nameVA < base) continue;
        nameOff = nameVA - base;
        if (nameOff + 9 > got) continue;
        {
            const char* nm = (const char*)(hbuf + nameOff);
            if (nm[0] == '$' && nm[1] == '$' && nm[2] == 'X' && nm[3] == 'T' &&
                nm[4] == 'I' && nm[5] == 'M' && nm[6] == 'A' && nm[7] == 'G' && nm[8] == 'E') {
                imgRaw = Rd32(hbuf + s + 0x0C);   /* file offset of section */
                imgSize = Rd32(hbuf + s + 0x10);
                break;
            }
        }
    }
    if (!imgRaw || imgSize < 0x20) { CloseHandle(h); return 0; }

    /* XPR0 header */
    SetFilePointer(h, imgRaw, NULL, FILE_BEGIN);
    if (!ReadFile(h, xpr, sizeof(xpr), &rd, NULL) || rd < 0x20) { CloseHandle(h); return 0; }
    if (xpr[0] != 'X' || xpr[1] != 'P' || xpr[2] != 'R' || xpr[3] != '0') { CloseHandle(h); return 0; }

    total = Rd32(xpr + 0x04);
    hsize = Rd32(xpr + 0x08);
    fmt = Rd32(xpr + 0x18);
    fcode = (fmt >> 8) & 0xFF;
    w = 1u << ((fmt >> 20) & 0xF);
    hh = 1u << ((fmt >> 24) & 0xF);
    (void)total;
    if (w == 0 || hh == 0 || w > 1024 || hh > 1024 || hsize < 0x20) { CloseHandle(h); return 0; }

    /* Always create as A8R8G8B8 (Xbox's swizzled 32-bit texture format; what
       CreateTexture reliably accepts and what DarkDash uses everywhere). DXT1
       stays DXT1. Unknown formats bail. */
    switch (fcode) {
    case 0x06: d3dfmt = D3DFMT_A8R8G8B8; pixBytes = w * hh * 4; break;  /* has alpha   */
    case 0x07: d3dfmt = D3DFMT_A8R8G8B8; pixBytes = w * hh * 4; break;  /* X8 -> force */
    case 0x0C: d3dfmt = D3DFMT_DXT1;     pixBytes = w * hh / 2; break;
    default:
        CloseHandle(h); return 0;
    }
    dataOff = imgRaw + hsize;

    /* The XPR0 pixel block is already in the GPU's native swizzled layout, and
       our texture is the same swizzled format -- so we read the bytes straight
       into the locked surface. No unswizzle, no DXT decode. */
    if (FAILED(dev->CreateTexture(w, hh, 1, 0, d3dfmt, D3DPOOL_MANAGED, &tex)) || !tex) {
        CloseHandle(h); return 0;
    }
    if (FAILED(tex->LockRect(0, &lr, NULL, 0))) { tex->Release(); CloseHandle(h); return 0; }

    SetFilePointer(h, dataOff, NULL, FILE_BEGIN);
    {
        BYTE* dst = (BYTE*)lr.pBits;
        DWORD done = 0, chunk;
        while (done < pixBytes) {
            if (!ReadFile(h, dst + done, pixBytes - done, &chunk, NULL) || chunk == 0) break;
            done += chunk;
        }
        /* X8R8G8B8 source: the 4th byte of each pixel is "don't care" (often 0),
           which would make the sprite blend out. Force full alpha. (Swizzle
           only reorders whole pixels, so every 4th byte is still an alpha.) */
        if (fcode == 0x07) {
            DWORD k;
            for (k = 3; k < pixBytes; k += 4) dst[k] = 0xFF;
        }
    }
    tex->UnlockRect(0);
    CloseHandle(h);

    out->tex = tex;
    out->w = (int)w;  out->h = (int)hh;
    out->pw = (int)w; out->ph = (int)hh;
    return 1;
}
/* Extract the raw $$XTIMAGE section (the XPR0 / TitleImage.xbx block) into 'out'
   (up to 'cap' bytes). Same section-find as Xbe_LoadTitleImage, but returns the
   raw block instead of decoding a texture -- used to copy a game's icon into a
   cert-patched attach.xbe. Returns 1 + *sizeOut on success; 0 if absent/too big. */
int Xbe_ExtractTitleXpr0(const char* xbePath, BYTE* out, DWORD cap, DWORD* sizeOut) {
    HANDLE h;
    BYTE   hbuf[XBE_SECHDR_READ];
    DWORD  got = 0, rd = 0;
    DWORD  base, nsec, secVA, secOff, imgRaw = 0, imgSize = 0, i;

    if (sizeOut) *sizeOut = 0;
    if (!xbePath || !out || cap == 0) return 0;

    h = CreateFile(xbePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(h, hbuf, XBE_SECHDR_READ, &got, NULL) || got < 0x140) { CloseHandle(h); return 0; }
    if (hbuf[0] != 'X' || hbuf[1] != 'B' || hbuf[2] != 'E' || hbuf[3] != 'H') { CloseHandle(h); return 0; }

    base = Rd32(hbuf + 0x104);
    nsec = Rd32(hbuf + 0x11C);
    secVA = Rd32(hbuf + 0x120);
    if (secVA < base || nsec == 0 || nsec > 64) { CloseHandle(h); return 0; }
    secOff = secVA - base;

    for (i = 0; i < nsec; i++) {
        DWORD s = secOff + i * 0x38;
        DWORD nameVA, nameOff;
        if (s + 0x38 > got) break;
        nameVA = Rd32(hbuf + s + 0x14);
        if (nameVA < base) continue;
        nameOff = nameVA - base;
        if (nameOff + 9 > got) continue;
        {
            const char* nm = (const char*)(hbuf + nameOff);
            if (nm[0] == '$' && nm[1] == '$' && nm[2] == 'X' && nm[3] == 'T' &&
                nm[4] == 'I' && nm[5] == 'M' && nm[6] == 'A' && nm[7] == 'G' && nm[8] == 'E') {
                imgRaw = Rd32(hbuf + s + 0x0C);   /* section file offset */
                imgSize = Rd32(hbuf + s + 0x10);
                break;
            }
        }
    }
    if (!imgRaw || imgSize < 0x20 || imgSize > cap) { CloseHandle(h); return 0; }

    SetFilePointer(h, imgRaw, NULL, FILE_BEGIN);
    if (!ReadFile(h, out, imgSize, &rd, NULL) || rd != imgSize) { CloseHandle(h); return 0; }
    CloseHandle(h);

    if (out[0] != 'X' || out[1] != 'P' || out[2] != 'R' || out[3] != '0') return 0;  /* must be XPR0 */
    if (sizeOut) *sizeOut = imgSize;
    return 1;
}