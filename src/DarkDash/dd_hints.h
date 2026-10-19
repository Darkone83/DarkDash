#pragma once
#ifndef DARKDASH_HINTS_H
#define DARKDASH_HINTS_H
/*--------------------------------------------------------------------------
    dd_hints -- shared, font-independent controller prompts.

    Input is an existing plain-text hint, e.g. "A SELECT  B BACK". Known
    controller names become painted glyphs; descriptive words keep the active
    .ddf font/theme colour. All measurements are in 640x480 virtual pixels.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Returns the virtual width that DD_Hints_Draw will consume, including gaps. */
    float DD_Hints_Measure(const char* hint);

    /* Draw inside an existing footer/dialog region (does NOT paint the footer art).
       Vertically centers both glyphs and font text in [top, top+height].
       Stops at a token boundary if width would exceed maxWidth. */
    void DD_Hints_Draw(IDirect3DDevice8* device, float x, float top,
        float maxWidth, float height, const char* hint, DWORD textColor);

    /* The shared themed footer. barTop and barHeight are screen-specific to
       preserve existing art placement (most screens 442/32; FileMan 452/24). */
    void DD_Hints_Footer(IDirect3DDevice8* device, float barTop, float barHeight,
        float hintX, float maxWidth, const char* hint, DWORD textColor);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_HINTS_H */
