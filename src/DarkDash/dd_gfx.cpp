/*---------------------------------------------------------------------------
    dd_gfx.cpp -- D3D8 device + frame lifecycle.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_gfx.h"
#include "dd_data.h"

static IDirect3D8* s_d3d = NULL;
static IDirect3DDevice8* s_device = NULL;
static int                s_width = 640;
static int                s_height = 480;

int Gfx_Init(void) {
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;

    if (s_device) return 1;

    s_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!s_d3d) return 0;

    ZeroMemory(&pp, sizeof(pp));
    /* Match the console's video setup. The UI is authored in a 640x480 logical
       space and UI_Init scales it (uniform + centered) to whatever backbuffer
       we create, so we can boot a real 1280x720 surface when the box is set to
       720p and the whole UI scales up crisply instead of being hardware-upscaled
       from 640x480. Falls back to 640x480 for SDTV/480p. */
    {
        DWORD vflags = XGetVideoFlags();
        DWORD vstd = XGetVideoStandard();
        int   pref = Data_Get()->videoRes;
        int   can720 = (vflags & XC_VIDEO_FLAGS_HDTV_720p) ? 1 : 0;
        int   use720;

        /* Auto follows the console; 720p honored only if the box can do it
           (forcing it on a 480-only box would produce no signal); 480p forces
           SDTV. */
        if (pref == DD_RES_480)      use720 = 0;
        else if (pref == DD_RES_720) use720 = can720;
        else                         use720 = can720;   /* AUTO */

        pp.Flags = D3DPRESENTFLAG_PROGRESSIVE;
        if (vflags & XC_VIDEO_FLAGS_WIDESCREEN)
            pp.Flags |= D3DPRESENTFLAG_WIDESCREEN;

        if (use720) { pp.BackBufferWidth = 1280; pp.BackBufferHeight = 720; }
        else { pp.BackBufferWidth = 640;  pp.BackBufferHeight = 480; }

        if (vstd == XC_VIDEO_STANDARD_PAL_I &&
            !(vflags & XC_VIDEO_FLAGS_PAL_60Hz))
            pp.FullScreen_RefreshRateInHz = 50;
        else
            pp.FullScreen_RefreshRateInHz = 60;
    }
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    hr = s_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &s_device);
    if (FAILED(hr)) {
        s_d3d->Release();
        s_d3d = NULL;
        return 0;
    }

    s_width = pp.BackBufferWidth;
    s_height = pp.BackBufferHeight;

    /* Sensible global defaults; per-draw modules set what they need. */
    s_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_device->SetRenderState(D3DRS_LIGHTING, FALSE);
    return 1;
}

void Gfx_Shutdown(void) {
    if (s_device) { s_device->Release(); s_device = NULL; }
    if (s_d3d) { s_d3d->Release();    s_d3d = NULL; }
}

IDirect3DDevice8* Gfx_Device(void) { return s_device; }
int Gfx_Width(void) { return s_width; }
int Gfx_Height(void) { return s_height; }

void Gfx_BeginFrame(DWORD clearColour) {
    if (!s_device) return;
    s_device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        clearColour, 1.0f, 0);
    s_device->BeginScene();
}

void Gfx_EndFrame(void) {
    if (!s_device) return;
    s_device->EndScene();
    s_device->Present(NULL, NULL, NULL, NULL);
}