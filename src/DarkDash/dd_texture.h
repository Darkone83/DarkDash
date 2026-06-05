#ifndef DARKDASH_TEXTURE_H
#define DARKDASH_TEXTURE_H
/*---------------------------------------------------------------------------
    dd_texture -- PNG -> swizzled A8R8G8B8 D3D8 texture (lodepng + XGSwizzleRect).

    DDS at runtime is slow (D3DX overhead + swizzle on load). PNG decodes via
    lodepng, we swap RGBA->BGRA, pad to power-of-two, swizzle, and upload.
    Linear (D3DFMT_LIN_*) locks the NV2A when sampled, so we always swizzle.

    Non-pow2 art is padded up; the real w/h are kept so sprites sample only
    the used sub-rect (u = w/pw, v = h/ph).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct {
        IDirect3DTexture8* tex;  /* NULL if unloaded */
        int w, h;               /* real image size  */
        int pw, ph;              /* padded pow2 size  */
    } Texture;

    /* Load a 32-bit PNG from 'path' into a swizzled texture.
       Returns 1 on success (out filled), 0 on failure (out zeroed). */
    int  Texture_LoadPNG(const char* path, Texture* out);

    /* JPEG via picojpeg (baseline). Same swizzled A8R8G8B8 output as LoadPNG.
       Wired and callable; format/usage decisions come later. */
    int  Texture_LoadJPEG(const char* path, Texture* out);

    /* Load an XPR0 (.xbx) texture (TitleImage.xbx / SaveImage.xbx). Handles DXT1
       and 32-bit A8R8G8B8 / X8R8G8B8 (swizzled or linear). 1 on success. */
    int  Texture_LoadXPR(const char* path, Texture* out);

    /* Build a soft radial glow texture (white, radial alpha falloff) of size x size
       (size should be power-of-two, e.g. 128). Draw it large + additive, tinted by
       the theme colour, for subtle ambient lighting. Returns 1 on success. */
    int  Texture_CreateRadialGlow(int size, Texture* out);

    void Texture_Release(Texture* t);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_TEXTURE_H */