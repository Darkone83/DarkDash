/*---------------------------------------------------------------------------
    dd_stbi.h -- C interface to the stb_image decoder (see dd_stbi.c).
    Decodes baseline/progressive JPEG and PNG from memory to RGBA8.
---------------------------------------------------------------------------*/
#ifndef DD_STBI_H
#define DD_STBI_H

#ifdef __cplusplus
extern "C" {
#endif

    /* Decode JPEG or PNG bytes (auto-detected) -> malloc'd RGBA8 (R,G,B,A). NULL on
       failure. Free the result with DD_StbFree. */
    unsigned char* DD_StbLoadImageMem(const unsigned char* data, int len, int* w, int* h);

    void DD_StbFree(void* p);

#ifdef __cplusplus
}
#endif

#endif /* DD_STBI_H */