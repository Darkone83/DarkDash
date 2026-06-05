/*---------------------------------------------------------------------------
    dd_preview.cpp -- see dd_preview.h.

    Non-blocking XMV playback skeleton built on XMVDecoder_GetNextFrame. The
    decoder renders each frame into an offscreen surface we own; Preview_Draw
    blits the current frame into a target rect via StretchRect. Pumped from the
    main loop -- no threads, no overlay takeover (unlike the blocking Play()
    intro path).

    SCAFFOLDING ONLY: compiled-in and callable. Audio (EnableAudioStream +
    periodic DirectSoundDoWork), the synopsis frame, file conventions, and the
    open affordance are deferred. Passing NULL for pTimeOfFrame lets the library
    self-synchronize, which is what we want for a simple per-frame pump.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xmv.h"
#include "dd_preview.h"
#include "dd_gfx.h"

static XMVDecoder* s_dec = NULL;
static IDirect3DSurface8* s_frame = NULL;   /* decode target, video-sized      */
static int                s_w = 0, s_h = 0;
static int                s_haveFrame = 0;   /* a NEWFRAME has landed at least once */
static int                s_state = PREVIEW_IDLE;

int Preview_IsOpen(void) { return s_dec != NULL; }

int Preview_Open(const char* path) {
    XMVVIDEO_DESC desc;
    HRESULT hr;
    IDirect3DDevice8* d = Gfx_Device();

    Preview_Close();   /* drop any prior session */
    if (!path || !d) return 0;

    hr = XMVDecoder_CreateDecoderForFile(XMVFLAG_NONE, path, &s_dec);
    if (FAILED(hr) || !s_dec) { s_dec = NULL; return 0; }

    ZeroMemory(&desc, sizeof(desc));
    XMVDecoder_GetVideoDescriptor(s_dec, &desc);
    if (desc.Width == 0 || desc.Height == 0) {     /* no video stream */
        XMVDecoder_CloseDecoder(s_dec); s_dec = NULL;
        return 0;
    }
    s_w = (int)desc.Width;
    s_h = (int)desc.Height;

    /* the decode target MUST be exactly the video geometry (per xmv.h). YUY2 is
       the format the decoder fills; create an image surface of that size. */
    hr = d->CreateImageSurface((UINT)s_w, (UINT)s_h, D3DFMT_YUY2, &s_frame);
    if (FAILED(hr) || !s_frame) {
        if (s_frame) { s_frame->Release(); s_frame = NULL; }
        XMVDecoder_CloseDecoder(s_dec); s_dec = NULL;
        return 0;
    }

    s_haveFrame = 0;
    s_state = PREVIEW_PLAYING;
    return 1;
}

void Preview_Close(void) {
    if (s_frame) { s_frame->Release(); s_frame = NULL; }
    if (s_dec) { XMVDecoder_CloseDecoder(s_dec); s_dec = NULL; }
    s_w = s_h = 0;
    s_haveFrame = 0;
    s_state = PREVIEW_IDLE;
}

int Preview_Tick(void) {
    XMVRESULT r = XMV_NOFRAME;
    HRESULT   hr;

    if (!s_dec || !s_frame) return PREVIEW_IDLE;

    /* NULL pTimeOfFrame => library self-syncs; we just pump each tick. */
    hr = XMVDecoder_GetNextFrame(s_dec, s_frame, &r, NULL);
    if (FAILED(hr) || r == XMV_FAIL) { s_state = PREVIEW_ERROR; return s_state; }

    if (r == XMV_NEWFRAME) { s_haveFrame = 1; s_state = PREVIEW_PLAYING; }
    else if (r == XMV_ENDOFFILE) s_state = PREVIEW_ENDED;
    /* XMV_NOFRAME: not yet time for the next frame; keep showing the last one */

    return s_state;
}

void Preview_Draw(float x, float y, float w, float h) {
    IDirect3DDevice8* d = Gfx_Device();
    IDirect3DSurface8* back = NULL;
    RECT dst;

    if (!s_frame || !s_haveFrame || !d) return;
    if (w <= 0.0f || h <= 0.0f) return;

    if (FAILED(d->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return;

    dst.left = (LONG)x;        dst.top = (LONG)y;
    dst.right = (LONG)(x + w);  dst.bottom = (LONG)(y + h);

    /* blit the decoded (YUY2) frame into the target rect; the GPU handles the
       YUY2->RGB conversion during StretchRect. Scaffolding: drawn straight to
       the back buffer rect. Panel compositing comes with the synopsis frame. */
    d->CopyRects(s_frame, NULL, 0, back, NULL);   /* placeholder: full-surface  */
    (void)dst;

    back->Release();
}

int Preview_Probe(const char* path) {
    XMVDecoder* dec = NULL;
    XMVVIDEO_DESC desc;
    HRESULT hr;
    int ok = 0;

    if (!path) return 0;
    hr = XMVDecoder_CreateDecoderForFile(XMVFLAG_NONE, path, &dec);
    if (SUCCEEDED(hr) && dec) {
        ZeroMemory(&desc, sizeof(desc));
        XMVDecoder_GetVideoDescriptor(dec, &desc);
        ok = (desc.Width != 0 && desc.Height != 0) ? 1 : 0;
        XMVDecoder_CloseDecoder(dec);
    }
    return ok;
}