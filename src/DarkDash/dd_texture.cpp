/*---------------------------------------------------------------------------
    dd_texture.cpp -- PNG -> swizzled A8R8G8B8 texture.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <xgraphics.h>
#include <stdlib.h>
#include <string.h>
#include "dd_texture.h"
#include "dd_gfx.h"
#include "lodepng.h"
#include "picojpeg.h"

/* next power of two >= v */
static unsigned dd_np2(unsigned v) {
    unsigned p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* Shared upload tail: take a tightly-packed w*h RGBA buffer, swap to BGRA,
   pad to power-of-two, swizzle, and upload as A8R8G8B8. Frees nothing; the
   caller owns rgba. Fills *out on success. Returns 1/0. Used by both the PNG
   and JPEG loaders so they share one code path. */
static int UploadRGBA(const unsigned char* rgba, unsigned w, unsigned h, Texture* out) {
    unsigned pw, ph, x, y;
    unsigned char* pad;
    IDirect3DTexture8* tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;

    if (!rgba || !out || w == 0 || h == 0) return 0;

    pw = dd_np2(w);
    ph = dd_np2(h);

    pad = (unsigned char*)malloc((size_t)pw * ph * 4);
    if (!pad) return 0;
    memset(pad, 0, (size_t)pw * ph * 4);

    for (y = 0; y < h; y++) {
        const unsigned char* src = rgba + (size_t)y * w * 4;
        unsigned char* dst = pad + (size_t)y * pw * 4;
        for (x = 0; x < w; x++) {
            dst[x * 4 + 0] = src[x * 4 + 2]; /* B */
            dst[x * 4 + 1] = src[x * 4 + 1]; /* G */
            dst[x * 4 + 2] = src[x * 4 + 0]; /* R */
            dst[x * 4 + 3] = src[x * 4 + 3]; /* A */
        }
    }

    hr = Gfx_Device()->CreateTexture(pw, ph, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
    if (FAILED(hr)) { free(pad); return 0; }

    hr = tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr)) { tex->Release(); free(pad); return 0; }

    XGSwizzleRect(pad, pw * 4, NULL, lr.pBits, pw, ph, NULL, 4);
    tex->UnlockRect(0);
    free(pad);

    out->tex = tex;
    out->w = (int)w;  out->h = (int)h;
    out->pw = (int)pw; out->ph = (int)ph;
    return 1;
}

int Texture_LoadPNG(const char* path, Texture* out) {
    unsigned char* rgba = NULL;
    unsigned w = 0, h = 0, err;
    int ok;

    if (out) { out->tex = NULL; out->w = out->h = out->pw = out->ph = 0; }
    if (!path || !out) return 0;

    /* lodepng gives RGBA byte order */
    err = lodepng_decode32_file(&rgba, &w, &h, path);
    if (err || !rgba) { if (rgba) free(rgba); return 0; }

    ok = UploadRGBA(rgba, w, h, out);
    free(rgba);
    return ok;
}

/* ---- JPEG loader (picojpeg) --------------------------------------------
   Scaffolding: a working baseline-JPEG decoder wired to the same upload path
   as PNG, so cover art / assets can use .jpg later. picojpeg pulls bytes via a
   callback; we feed it from a file. It decodes MCU-by-MCU into 8x8 component
   blocks which we assemble into a full RGBA image. */

typedef struct {
    HANDLE h;
} JpegSrc;

static unsigned char Jpeg_NeedBytes(unsigned char* pBuf, unsigned char bufSize,
    unsigned char* pBytesRead, void* pData) {
    JpegSrc* s = (JpegSrc*)pData;
    DWORD got = 0;
    if (!ReadFile(s->h, pBuf, (DWORD)bufSize, &got, NULL)) { *pBytesRead = 0; return PJPG_STREAM_READ_ERROR; }
    *pBytesRead = (unsigned char)got;
    return 0;   /* 0 = ok */
}

int Texture_LoadJPEG(const char* path, Texture* out) {
    JpegSrc src;
    pjpeg_image_info_t info;
    unsigned char* rgba = NULL;
    int ok = 0, mcuX = 0, mcuY = 0;
    unsigned w, h;

    if (out) { out->tex = NULL; out->w = out->h = out->pw = out->ph = 0; }
    if (!path || !out) return 0;

    src.h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (src.h == INVALID_HANDLE_VALUE) return 0;

    if (pjpeg_decode_init(&info, Jpeg_NeedBytes, &src, 0) != 0) {
        CloseHandle(src.h);
        return 0;
    }

    w = (unsigned)info.m_width;
    h = (unsigned)info.m_height;
    rgba = (unsigned char*)malloc((size_t)w * h * 4);
    if (!rgba) { CloseHandle(src.h); return 0; }
    memset(rgba, 0, (size_t)w * h * 4);

    /* Walk every MCU. Each MCU is m_MCUWidth x m_MCUHeight px, delivered as
       (MCUW/8)*(MCUH/8) 8x8 blocks in m_pMCUBufR/G/B. For greyscale, only R is
       valid (use it for G/B too). Place each pixel into the RGBA buffer. */
    for (;;) {
        unsigned char status = pjpeg_decode_mcu();
        int bx, by, blk;

        if (status) {
            if (status == PJPG_NO_MORE_BLOCKS) break;   /* done */
            free(rgba); CloseHandle(src.h); return 0;   /* decode error */
        }

        /* destination top-left of this MCU in the image */
        {
            int dstX0 = mcuX * info.m_MCUWidth;
            int dstY0 = mcuY * info.m_MCUHeight;
            int blocksPerRow = info.m_MCUWidth >> 3;     /* /8 */
            int blockRows = info.m_MCUHeight >> 3;

            for (by = 0; by < blockRows; by++) {
                for (bx = 0; bx < blocksPerRow; bx++) {
                    int srcOff = (by * blocksPerRow + bx) * 64;  /* 8x8 block base */
                    int px, py;
                    blk = srcOff;
                    for (py = 0; py < 8; py++) {
                        for (px = 0; px < 8; px++) {
                            int ix = dstX0 + bx * 8 + px;
                            int iy = dstY0 + by * 8 + py;
                            int si = blk + py * 8 + px;
                            unsigned char* d;
                            if (ix >= (int)w || iy >= (int)h) continue;  /* edge MCU overhang */
                            d = rgba + ((size_t)iy * w + ix) * 4;
                            if (info.m_comps == 1) {
                                unsigned char g = info.m_pMCUBufR[si];
                                d[0] = g; d[1] = g; d[2] = g; d[3] = 255;
                            }
                            else {
                                d[0] = info.m_pMCUBufR[si];
                                d[1] = info.m_pMCUBufG[si];
                                d[2] = info.m_pMCUBufB[si];
                                d[3] = 255;
                            }
                        }
                    }
                }
            }
        }

        if (++mcuX == info.m_MCUSPerRow) { mcuX = 0; mcuY++; }
    }

    CloseHandle(src.h);
    ok = UploadRGBA(rgba, w, h, out);
    free(rgba);
    return ok;
}

/* ---- XPR0 (.xbx) texture loader ----------------------------------------
   Parses an XPR0 container (TitleImage.xbx / SaveImage.xbx), decodes the first
   texture resource into BGRA, and uploads it swizzled like Texture_LoadPNG.
   Handles the common title-image formats: DXT1 (0x0C, linear block data) and
   32-bit A8R8G8B8 / X8R8G8B8 in both swizzled (0x06/0x07) and linear
   (0x12/0x1E) variants. Other formats are declined (returns 0).
---------------------------------------------------------------------------- */

static unsigned Rd32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
        ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* decode one DXT1 block (8 bytes) into a 4x4 BGRA tile written into dst at
   (bx,by) within a row-pitched BGRA image of width imgW. */
static void Dxt1Block(const unsigned char* blk, unsigned char* dst,
    int imgW, int imgH, int bx, int by) {
    unsigned c0 = (unsigned)blk[0] | ((unsigned)blk[1] << 8);
    unsigned c1 = (unsigned)blk[2] | ((unsigned)blk[3] << 8);
    unsigned bits = Rd32(blk + 4);
    int r[4], g[4], b[4], i, px, py;

    /* expand RGB565 endpoints */
    r[0] = (int)((c0 >> 11) & 0x1F) * 255 / 31;
    g[0] = (int)((c0 >> 5) & 0x3F) * 255 / 63;
    b[0] = (int)(c0 & 0x1F) * 255 / 31;
    r[1] = (int)((c1 >> 11) & 0x1F) * 255 / 31;
    g[1] = (int)((c1 >> 5) & 0x3F) * 255 / 63;
    b[1] = (int)(c1 & 0x1F) * 255 / 31;
    if (c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
    }
    else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
        r[3] = 0; g[3] = 0; b[3] = 0;   /* index 3 = transparent black (DXT1 1-bit alpha) */
    }
    for (py = 0; py < 4; py++) {
        for (px = 0; px < 4; px++) {
            int xx = bx + px, yy = by + py;
            unsigned char* o;
            if (xx >= imgW || yy >= imgH) continue;
            i = (int)((bits >> (2 * (py * 4 + px))) & 3);
            o = dst + ((size_t)yy * imgW + xx) * 4;
            o[0] = (unsigned char)b[i];
            o[1] = (unsigned char)g[i];
            o[2] = (unsigned char)r[i];
            o[3] = (c0 <= c1 && i == 3) ? 0 : 255;
        }
    }
}

int Texture_LoadXPR(const char* path, Texture* out) {
    HANDLE h;
    unsigned char* file = NULL;
    DWORD fsize = 0, got = 0;
    unsigned hdrSize, fmt, dataOff;
    int w = 0, ht = 0;
    unsigned char* bgra = NULL;
    unsigned pw, ph;
    unsigned char* pad = NULL;
    IDirect3DTexture8* tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;
    int ok = 0;

    if (out) { out->tex = NULL; out->w = out->h = out->pw = out->ph = 0; }
    if (!path || !out) return 0;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    fsize = GetFileSize(h, NULL);
    if (fsize < 32 || fsize >(8 * 1024 * 1024)) { CloseHandle(h); return 0; }
    file = (unsigned char*)malloc(fsize);
    if (!file) { CloseHandle(h); return 0; }
    if (!ReadFile(h, file, fsize, &got, NULL) || got != fsize) { CloseHandle(h); free(file); return 0; }
    CloseHandle(h);

    /* XPR0 image header (per XBMC4Gamers' XPR0_image layout):
         0x00 magic "XPR0"
         0x08 headerSize        -- texture data starts HERE (== 2048 typically)
         0x19 textureFormat     -- D3DFMT_* code (0x0C DXT1, 0x06/0x07 32-bit...)
         0x1A packedLevelWidth  -- width  = 1 << (byte >> 4)
         0x1B packedHeightMisc  -- height = 1 << (byte >> 4)
       My earlier reading of the format/dims from a "gpu" dword at +24 was the
       bug (gave bogus 128x1); these are single bytes at 0x19-0x1B. */
    if (file[0] != 'X' || file[1] != 'P' || file[2] != 'R' || file[3] != '0') { free(file); return 0; }
    hdrSize = Rd32(file + 8);
    fmt = file[0x19];

    {
        /* Two documented encodings of the level exponents; try both and keep
           whichever yields plausible power-of-2 icon dimensions:
             (a) xboxdevwiki: gpu dword @0x18 -> width exp = bits24-27 (0x1B low
                 nibble), height exp = bits28-31 (0x1B high nibble).
             (b) XBMC limpp: width level = byte 0x1A >> 4, icons square.
           The raw bytes are shown in the on-screen debug so this can be pinned
           exactly on hardware. */
        unsigned b1A = file[0x1A];
        unsigned b1B = file[0x1B];
        unsigned aW = b1B & 0x0F, aH = (b1B >> 4) & 0x0F;       /* (a) */
        unsigned bW = (b1A >> 4) & 0x0F;                         /* (b) */
        int wa, ha, wb;
        wa = (aW >= 1 && aW <= 10) ? (1 << aW) : 0;
        ha = (aH >= 1 && aH <= 10) ? (1 << aH) : 0;
        wb = (bW >= 1 && bW <= 10) ? (1 << bW) : 0;
        if (wa > 0 && ha > 0) { w = wa; ht = ha; }             /* prefer (a) */
        else if (wb > 0) { w = wb; ht = wb; }             /* (b), square */
        else { w = 0;  ht = 0; }
    }

    /* texture data starts at headerSize (confirmed against XBMC + on-HW: 2048) */
    dataOff = hdrSize;
    if (dataOff == 0 || dataOff >= fsize) { free(file); return 0; }

    if (w <= 1 || ht <= 1 || w > 1024 || ht > 1024) { free(file); return 0; }

    bgra = (unsigned char*)malloc((size_t)w * ht * 4);
    if (!bgra) { free(file); return 0; }
    memset(bgra, 0, (size_t)w * ht * 4);

    if (fmt == 0x0C) {
        /* DXT1: linear 4x4 blocks, 8 bytes each, row-major in blocks */
        const unsigned char* src = file + dataOff;
        int bx, by, blocksW = (w + 3) / 4, blocksH = (ht + 3) / 4;
        size_t need = (size_t)blocksW * blocksH * 8;
        if (dataOff + need <= fsize) {
            for (by = 0; by < blocksH; by++)
                for (bx = 0; bx < blocksW; bx++)
                    Dxt1Block(src + ((size_t)by * blocksW + bx) * 8, bgra, w, ht, bx * 4, by * 4);
            ok = 1;
        }
    }
    else if (fmt == 0x0E || fmt == 0x0F) {
        /* DXT3 (0x0E) / DXT5 (0x0F): 16-byte blocks = 8-byte alpha block then
           an 8-byte DXT1-style colour block. Decode colour via Dxt1Block (the
           colour endpoints are treated as c0>c1 mode -> no 1-bit alpha), then
           overlay the alpha. We approximate DXT5 alpha with the two endpoints
           (good enough for save icons); DXT3 uses the 4-bit explicit alpha. */
        const unsigned char* src = file + dataOff;
        int bx, by, blocksW = (w + 3) / 4, blocksH = (ht + 3) / 4;
        size_t need = (size_t)blocksW * blocksH * 16;
        if (dataOff + need <= fsize) {
            for (by = 0; by < blocksH; by++) {
                for (bx = 0; bx < blocksW; bx++) {
                    const unsigned char* blk = src + ((size_t)by * blocksW + bx) * 16;
                    const unsigned char* col = blk + 8;     /* colour half */
                    int px, py;
                    /* colour: force opaque (alpha overwritten below). Use a
                       local copy with c0>c1 semantics by calling Dxt1Block. */
                    Dxt1Block(col, bgra, w, ht, bx * 4, by * 4);
                    /* alpha */
                    for (py = 0; py < 4; py++) {
                        for (px = 0; px < 4; px++) {
                            int xx = bx * 4 + px, yy = by * 4 + py, a;
                            unsigned char* o;
                            if (xx >= w || yy >= ht) continue;
                            o = bgra + ((size_t)yy * w + xx) * 4;
                            if (fmt == 0x0E) {
                                /* DXT3: 4 bits per pixel, 64 bits total */
                                int idx = py * 4 + px;
                                int nyb = (blk[idx >> 1] >> ((idx & 1) * 4)) & 0x0F;
                                a = nyb * 255 / 15;
                            }
                            else {
                                /* DXT5: 3-bit indices into 8 alpha values from
                                   two endpoints. 48 index bits split across two
                                   32-bit reads (MSVC2003-safe, no 64-bit shift). */
                                int a0 = blk[0], a1 = blk[1];
                                int bit = 3 * (py * 4 + px);
                                unsigned lo = (unsigned)blk[2] | ((unsigned)blk[3] << 8) |
                                    ((unsigned)blk[4] << 16) | ((unsigned)blk[5] << 24);
                                unsigned hi = (unsigned)blk[6] | ((unsigned)blk[7] << 8);
                                int idx, av;
                                if (bit < 32) {
                                    idx = (int)((lo >> bit) & 7);
                                    if (bit > 29) idx |= (int)((hi << (32 - bit)) & 7);
                                }
                                else {
                                    idx = (int)((hi >> (bit - 32)) & 7);
                                }
                                if (a0 > a1) {
                                    static const int num[8] = { 7,0,6,5,4,3,2,1 };
                                    av = (idx == 0) ? a0 : (idx == 1) ? a1
                                        : (num[idx] * a0 + (7 - num[idx]) * a1) / 7;
                                }
                                else {
                                    if (idx == 0) av = a0;
                                    else if (idx == 1) av = a1;
                                    else if (idx == 6) av = 0;
                                    else if (idx == 7) av = 255;
                                    else { static const int num5[8] = { 0,0,5,4,3,2,0,0 }; av = (num5[idx] * a0 + (5 - num5[idx]) * a1) / 5; }
                                }
                                a = av;
                            }
                            o[3] = (unsigned char)a;
                        }
                    }
                }
            }
            ok = 1;
        }
    }
    else if (fmt == 0x06 || fmt == 0x07 || fmt == 0x12 || fmt == 0x1E) {
        /* 32-bit A8R8G8B8 / X8R8G8B8. 0x06/0x07 swizzled (SZ), 0x12/0x1E
           linear (LU). Source is already BGRA byte order on the Xbox. */
        const unsigned char* src = file + dataOff;
        size_t need = (size_t)w * ht * 4;
        int sw = (fmt == 0x06 || fmt == 0x07);     /* needs unswizzle */
        if (dataOff + need <= fsize) {
            if (!sw) {
                memcpy(bgra, src, need);
            }
            else {
                XGUnswizzleRect((void*)src, (UINT)w, (UINT)ht, NULL,
                    bgra, w * 4, NULL, 4);
            }
            if (fmt == 0x07 || fmt == 0x1E) {       /* X8: force opaque */
                int i, n = w * ht;
                for (i = 0; i < n; i++) bgra[i * 4 + 3] = 255;
            }
            ok = 1;
        }
    }
    free(file);
    if (!ok) { free(bgra); return 0; }

    /* upload: pad to pow2, swizzle, like Texture_LoadPNG */
    pw = dd_np2((unsigned)w);
    ph = dd_np2((unsigned)ht);
    pad = (unsigned char*)malloc((size_t)pw * ph * 4);
    if (!pad) { free(bgra); return 0; }
    memset(pad, 0, (size_t)pw * ph * 4);
    {
        int y;
        for (y = 0; y < ht; y++)
            memcpy(pad + (size_t)y * pw * 4, bgra + (size_t)y * w * 4, (size_t)w * 4);
    }
    free(bgra);

    hr = Gfx_Device()->CreateTexture(pw, ph, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
    if (FAILED(hr)) { free(pad); return 0; }
    hr = tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr)) { tex->Release(); free(pad); return 0; }
    XGSwizzleRect(pad, pw * 4, NULL, lr.pBits, pw, ph, NULL, 4);
    tex->UnlockRect(0);
    free(pad);

    out->tex = tex;
    out->w = w;  out->h = ht;
    out->pw = (int)pw; out->ph = (int)ph;
    return 1;
}

int Texture_CreateRadialGlow(int size, Texture* out) {
    int pw, ph, x, y, cx, cy, r2;
    unsigned char* buf;
    IDirect3DTexture8* tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;

    if (out) { out->tex = NULL; out->w = out->h = out->pw = out->ph = 0; }
    if (!out || size <= 0) return 0;

    pw = size; ph = size; cx = size / 2; cy = size / 2;
    r2 = (size / 2) * (size / 2);

    buf = (unsigned char*)malloc((size_t)pw * ph * 4);
    if (!buf) return 0;

    /* white RGB, radial alpha (integer math -> no float->int / Ftoi) */
    for (y = 0; y < ph; y++) {
        for (x = 0; x < pw; x++) {
            int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy, a;
            unsigned char* p = buf + ((size_t)y * pw + x) * 4;
            if (d2 >= r2) a = 0;
            else { a = 255 * (r2 - d2) / r2; a = (a * a) / 255; }  /* soft falloff */
            p[0] = 255; p[1] = 255; p[2] = 255; p[3] = (unsigned char)a;
        }
    }

    hr = Gfx_Device()->CreateTexture(pw, ph, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
    if (FAILED(hr)) { free(buf); return 0; }
    hr = tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr)) { tex->Release(); free(buf); return 0; }
    XGSwizzleRect(buf, pw * 4, NULL, lr.pBits, pw, ph, NULL, 4);
    tex->UnlockRect(0);
    free(buf);

    out->tex = tex;
    out->w = pw; out->h = ph; out->pw = pw; out->ph = ph;
    return 1;
}

void Texture_Release(Texture* t) {
    if (t && t->tex) { t->tex->Release(); t->tex = NULL; }
}