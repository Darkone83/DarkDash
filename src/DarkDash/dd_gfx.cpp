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
static char               s_videoMode[8] = "480i";   /* set during Gfx_Init */

static void GfxSetMode(const char* m) {
    int i = 0;
    while (m[i] && i < (int)sizeof(s_videoMode) - 1) { s_videoMode[i] = m[i]; i++; }
    s_videoMode[i] = 0;
}

const char* Gfx_VideoModeStr(void) { return s_videoMode; }

int Gfx_Init(void) {
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;

    if (s_device) return 1;

    s_d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!s_d3d) return 0;

    ZeroMemory(&pp, sizeof(pp));
    /* Mode selection mirrors XbDiag's proven, 1.6-safe logic. The key point:
       a standard-def display runs 480i (or 576i PAL) and the backbuffer MUST be
       requested with D3DPRESENTFLAG_INTERLACED -- only true 480p/720p HD modes
       are PROGRESSIVE. Forcing PROGRESSIVE on an interlaced SDTV box (common on
       rev 1.6 / Xcalibur) makes CreateDevice fail, so the dash exits to a black
       screen. The UI is authored at 640x480 and UI_Init scales it to whatever
       backbuffer we create. 'pref' lets Settings force 480/720; AUTO follows
       the console's reported capabilities. */
    {
        DWORD vflags = XGetVideoFlags();
        DWORD vstd = XGetVideoStandard();
        int   pref = Data_Get()->videoRes;
        int   has720 = (vflags & XC_VIDEO_FLAGS_HDTV_720p) ? 1 : 0;
        int   has480p = (vflags & XC_VIDEO_FLAGS_HDTV_480p) ? 1 : 0;
        int   palI = (vstd == XC_VIDEO_STANDARD_PAL_I) ? 1 : 0;
        int   pal60 = (vflags & XC_VIDEO_FLAGS_PAL_60Hz) ? 1 : 0;
        int   use720;

        /* 480p forces SDTV res; 720p honored only if the box reports it
           (forcing it on a 480-only box yields no signal); AUTO follows box. */
        if (pref == DD_RES_480)      use720 = 0;
        else if (pref == DD_RES_720) use720 = has720;
        else                         use720 = has720;   /* AUTO */

        pp.FullScreen_RefreshRateInHz = 60;

        if (use720) {
            /* 720p: progressive widescreen HD */
            pp.BackBufferWidth = 1280;
            pp.BackBufferHeight = 720;
            pp.Flags = D3DPRESENTFLAG_PROGRESSIVE | D3DPRESENTFLAG_WIDESCREEN;
            GfxSetMode("720p");
        }
        else if (has480p && pref != DD_RES_480) {
            /* 480p: progressive SDTV (only when the box actually reports 480p) */
            pp.BackBufferWidth = 640;
            pp.BackBufferHeight = 480;
            pp.Flags = D3DPRESENTFLAG_PROGRESSIVE;
            if (vflags & XC_VIDEO_FLAGS_WIDESCREEN) pp.Flags |= D3DPRESENTFLAG_WIDESCREEN;
            GfxSetMode("480p");
        }
        else if (palI && !pal60) {
            /* true PAL-I: 576i 50Hz interlaced, 4:3 (640x576, not 720) */
            pp.BackBufferWidth = 640;
            pp.BackBufferHeight = 576;
            pp.Flags = D3DPRESENTFLAG_INTERLACED;
            pp.FullScreen_RefreshRateInHz = 50;
            GfxSetMode("576i");
        }
        else {
            /* 480i baseline -- NTSC, PAL-M, PAL60. Interlaced is REQUIRED. */
            pp.BackBufferWidth = 640;
            pp.BackBufferHeight = 480;
            pp.Flags = D3DPRESENTFLAG_INTERLACED;
            GfxSetMode("480i");
        }
    }
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    hr = s_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &s_device);

    /* Safe-mode fallback: if the preferred mode is rejected for any reason,
       retry with the most conservative mode every Xbox can produce -- 640x480
       interlaced, 60Hz -- rather than returning 0 (which exits the dash to a
       black screen). Keeps a 50Hz PAL box on 50Hz. */
    if (FAILED(hr)) {
        DWORD vstd2 = XGetVideoStandard();
        DWORD vflag2 = XGetVideoFlags();
        pp.BackBufferWidth = 640;
        pp.BackBufferHeight = 480;
        pp.Flags = D3DPRESENTFLAG_INTERLACED;
        if (vstd2 == XC_VIDEO_STANDARD_PAL_I && !(vflag2 & XC_VIDEO_FLAGS_PAL_60Hz)) {
            pp.BackBufferHeight = 576;
            pp.FullScreen_RefreshRateInHz = 50;
            GfxSetMode("576i");
        }
        else {
            pp.FullScreen_RefreshRateInHz = 60;
            GfxSetMode("480i");
        }
        hr = s_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &s_device);
    }
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