#ifndef DARKDASH_GFX_H
#define DARKDASH_GFX_H
/*---------------------------------------------------------------------------
    dd_gfx -- D3D8 device creation and frame lifecycle for DarkDash.

    Owns the IDirect3DDevice8. Everything else (ui, texture, font) draws
    through the device exposed by Gfx_Device(). Basic bring-up runs at a
    fixed 640x480; widescreen / 720p detection is a later step (noted inline).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Create the D3D8 device. Returns 1 on success, 0 on failure. */
    int  Gfx_Init(void);
    void Gfx_Shutdown(void);

    IDirect3DDevice8* Gfx_Device(void);
    int  Gfx_Width(void);    /* backbuffer width  in pixels */
    int  Gfx_Height(void);   /* backbuffer height in pixels */
    const char* Gfx_VideoModeStr(void);   /* "480i"/"480p"/"576i"/"720p" */

    /* Clear to 'clearColour' (ARGB) and open the scene. */
    void Gfx_BeginFrame(DWORD clearColour);
    /* Close the scene and present. */
    void Gfx_EndFrame(void);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_GFX_H */