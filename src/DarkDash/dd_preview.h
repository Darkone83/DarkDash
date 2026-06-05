#pragma once
/*---------------------------------------------------------------------------
    dd_preview.h -- XMV preview-video playback (scaffolding).

    This is the plumbing for in-panel preview videos, retooled from the proven
    XMV engine in XBCraft / ScorchedEarthXB but built around the NON-BLOCKING
    XMVDecoder_GetNextFrame path (decodes into a surface we own, so it can be
    drawn inside a panel sub-rect and pumped from the main loop) rather than the
    blocking XMVDecoder_Play used for intros.

    SCOPE: this pass only makes the engine compiled-in and callable. The synopsis
    frame, file conventions (_resources\preview.xmv), audio handoff, and the
    WHITE-to-open affordance are all LATER work. Nothing here is wired into a
    screen yet -- these functions exist so that work has a foundation.

    Lifecycle when used later:
        Preview_Open(path)   -> create decoder + frame surface
        Preview_Tick()       -> call each frame; decodes next frame if due
        Preview_Draw(x,y,w,h)-> blit the current frame into a rect
        Preview_Close()      -> tear down
---------------------------------------------------------------------------*/
#ifndef DD_PREVIEW_H
#define DD_PREVIEW_H

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* result of the last Tick, for callers to gate UI on */
    enum {
        PREVIEW_IDLE = 0,   /* not open                         */
        PREVIEW_PLAYING,    /* open, frames coming              */
        PREVIEW_ENDED,      /* reached end of file              */
        PREVIEW_ERROR       /* decode/IO failure                */
    };

    int  Preview_Open(const char* path);   /* 1 = decoder up, 0 = failed/unavailable */
    void Preview_Close(void);
    int  Preview_IsOpen(void);

    /* Pump the decoder. Call once per frame while open. Returns a PREVIEW_* state.
       Non-blocking: decodes at most the frame that's due now. */
    int  Preview_Tick(void);

    /* Draw the most recently decoded frame into the given rect (virtual coords).
       No-op if no frame has been decoded yet. */
    void Preview_Draw(float x, float y, float w, float h);

    /* True if a probe of `path` looks like a usable preview (exists + opens). Lets
       a caller decide whether to show the preview affordance at all. */
    int  Preview_Probe(const char* path);

#ifdef __cplusplus
}
#endif
#endif /* DD_PREVIEW_H */