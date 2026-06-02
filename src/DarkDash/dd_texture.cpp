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

/* next power of two >= v */
static unsigned dd_np2(unsigned v) {
    unsigned p = 1;
    while (p < v) p <<= 1;
    return p;
}

int Texture_LoadPNG(const char* path, Texture* out) {
    unsigned char* rgba = NULL;
    unsigned w = 0, h = 0, err;
    unsigned pw, ph, x, y;
    unsigned char* pad;
    IDirect3DTexture8* tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;

    if (out) { out->tex = NULL; out->w = out->h = out->pw = out->ph = 0; }
    if (!path || !out) return 0;

    /* lodepng gives RGBA byte order */
    err = lodepng_decode32_file(&rgba, &w, &h, path);
    if (err || !rgba) { if (rgba) free(rgba); return 0; }

    pw = dd_np2(w);
    ph = dd_np2(h);

    pad = (unsigned char*)malloc((size_t)pw * ph * 4);
    if (!pad) { free(rgba); return 0; }
    memset(pad, 0, (size_t)pw * ph * 4);

    /* copy rows into the pow2 buffer, swapping R<->B (RGBA -> BGRA).
       BGRA byte order is what D3DFMT_A8R8G8B8 wants in memory. */
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
    free(rgba);

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