/*---------------------------------------------------------------------------
    dd_stbi.c -- baseline+progressive JPEG and PNG decode for DarkDash via
    stb_image.

    Replaces the two former decoders: picojpeg (baseline-only JPEG) and lodepng
    (PNG). stb_image decodes baseline AND progressive JPEG plus the full range of
    PNG (8/16-bit, palette, grayscale, interlaced), bringing its own zlib inflate
    so PNG needs no external dependency. stbi_load_from_memory auto-detects the
    format from the file's magic bytes, so one entry point serves both.

    The stb implementation lives in this single C translation unit so the ~8k
    lines stay out of the C++ units. RXDK-safe configuration:

      STBI_NO_SIMD          - pure C; no <emmintrin.h>/SSE intrinsics (MSVC 2003
                              auto-enables SSE2 only for >= VC2005, but we force
                              it off so the build never pulls intrinsics in).
      STBI_ONLY_JPEG /
      STBI_ONLY_PNG         - compile just these two decoders (+ the PNG zlib).
      STBI_NO_STDIO         - we hand it a memory buffer; no fopen/FILE.
      STBI_NO_HDR /
      STBI_NO_LINEAR        - excludes the only pow()/ldexp() float paths.
      STBI_NO_THREAD_LOCALS - no thread_local (unavailable on MSVC 2003).
      STBI_ASSERT(x)        - disabled; no <assert.h> dependency.

    Float-to-int casts inside stb resolve through DarkDash's existing
    __ftol2_sse shim (dd_ftol.cpp), same as minimp3.
---------------------------------------------------------------------------*/
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"

#include "dd_stbi.h"

/* Decode a JPEG or PNG (format auto-detected) from a memory buffer to a tightly
   packed RGBA8 image (R,G,B,A byte order -- exactly what UploadRGBA wants).
   Returns a malloc'd buffer the caller frees with DD_StbFree, or NULL on
   failure. *w/*h receive the dimensions. */
unsigned char* DD_StbLoadImageMem(const unsigned char* data, int len, int* w, int* h)
{
    int comp = 0;
    if (!data || len <= 0 || !w || !h) return 0;
    return stbi_load_from_memory(data, len, w, h, &comp, 4);
}

void DD_StbFree(void* p)
{
    if (p) stbi_image_free(p);
}