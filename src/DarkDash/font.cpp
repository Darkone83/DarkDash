/*---------------------------------------------------------------------------
    font.cpp -- Font atlas texture loader and glyph renderer.

    Loads the pre-baked font_atlas.h (RGBA byte array + GlyphMetrics tables)
    into a D3D8 texture, then blits individual glyphs via textured quads.
    Uses alpha-blending so glyphs composite correctly over any background.
---------------------------------------------------------------------------*/

#include <xtl.h>
#include "font.h"
#include "dd_ui.h"
#include "font_atlas.h"
#include <xgraphics.h>
#include <string.h>
#include <stdlib.h>

/*    Internal state                                                           */

static IDirect3DTexture8* s_atlas_tex = NULL;

/* Active glyph metrics per size. Point at the baked tables by default; a loaded
   .ddf swaps these to point at s_loadedMetrics instead. */
static const GlyphMetrics* s_metrics[3] = {
    g_glyphsSmall,
    g_glyphsMedium,
    g_glyphsLarge
};

/* storage for a loaded custom font's metrics (3 sizes x 95 glyphs) */
static GlyphMetrics s_loadedMetrics[3][95];
static int          s_usingCustom = 0;   /* 1 = a .ddf is live, 0 = baked Default */

/* Layout scale per size = virtual_size / raster_size. The baked Default is
   rasterized at virtual sizes (scale 1.0). A custom .ddf is rasterized at
   720p sizes for crispness, so its atlas pixels are ~1.5x larger; we scale the
   LAYOUT footprint (advance, quad w/h, bearing) back down to virtual units so
   text occupies the same space Default did -- no clipping -- while still
   sampling the hi-res atlas pixels for the glyph image. */
static float s_layoutScale[3] = { 1.0f, 1.0f, 1.0f };
static const int k_virtSize[3] = { FONT_SMALL_SIZE, FONT_MEDIUM_SIZE, FONT_LARGE_SIZE };

/* Glyph heights per size (filled during init from metrics) */
static int s_glyph_h[3] = { FONT_SMALL_SIZE, FONT_MEDIUM_SIZE, FONT_LARGE_SIZE };
/* Max ascender per size -- largest -bear_y across all glyphs */
static int s_max_ascender[3] = { 0, 0, 0 };

/* Atlas dimensions -- runtime now (baked size by default; a .ddf overrides). */
static int s_atlasW = FONT_ATLAS_WIDTH;
static int s_atlasH = FONT_ATLAS_HEIGHT;
#define ATLAS_W  s_atlasW
#define ATLAS_H  s_atlasH

/*    Vertex format                                                            */

#define FONT_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

typedef struct {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
} FontVert;

/* World-space (transformed) glyph vertex for iso-plane text. */
typedef struct {
    float x, y, z;
    DWORD colour;
    float u, v;
} FontIsoVert;

/*    Init / Shutdown                                                          */

/* (re)compute glyph heights + max ascender from whatever s_metrics points at,
   expressed in VIRTUAL units (atlas-pixel metrics * layout scale).

   IMPORTANT: always assign s_glyph_h / s_max_ascender (never skip), so a prior
   font's values can't persist when switching fonts. The baked Default font uses
   a positive-bear_y convention (so -bear_y <= 0 and the ascender clamps to 0,
   which is how Default is tuned); custom .ddf fonts use negative bear_y (top
   above baseline) so -bear_y is the upward extent. Clamping at 0 handles both. */
static void RecomputeMetrics(void) {
    int sz, i;
    for (sz = 0; sz < 3; sz++) {
        int maxh = 0, maxasc = 0, maxdesc = 0;
        for (i = 0; i < 95; i++) {
            int asc, desc;
            if (s_metrics[sz][i].h > maxh) maxh = s_metrics[sz][i].h;
            /* top above baseline: custom .ddf use negative bear_y, so -bear_y
               is the upward extent (clamped >=0 for Default's model too). */
            asc = -s_metrics[sz][i].bear_y;
            if (asc > maxasc) maxasc = asc;
            /* bottom below baseline: baseline + bear_y is the glyph top, +h is
               its bottom; the lowest such bottom is the descender extent. */
            desc = s_metrics[sz][i].bear_y + s_metrics[sz][i].h;
            if (desc > maxdesc) maxdesc = desc;
        }
        s_max_ascender[sz] = (int)((float)maxasc * s_layoutScale[sz]);
        /* True line box = full ascender-to-descender extent, not just the single
           tallest glyph. A font whose tallest glyph and lowest-descending glyph
           differ would otherwise under-report height and clip 1-2px at the
           bottom of a row. Fall back to maxh if the bear_y model gives less. */
        {
            int box = maxasc + maxdesc;
            if (box < maxh) box = maxh;
            s_glyph_h[sz] = (int)((float)box * s_layoutScale[sz]);
        }
    }
}

/* Build the atlas texture from a BGRA pixel buffer of the given dims. Releases
   any previous texture only on success, so a failed load leaves the old font
   intact. Returns 1 on success. */
static int BuildAtlasTexture(IDirect3DDevice8* pDevice,
    const unsigned char* bgra, int w, int h) {
    IDirect3DTexture8* tex = NULL;
    D3DLOCKED_RECT lr;
    HRESULT hr;

    hr = pDevice->CreateTexture((UINT)w, (UINT)h, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex);
    if (FAILED(hr)) return 0;

    hr = tex->LockRect(0, &lr, NULL, 0);
    if (FAILED(hr)) { tex->Release(); return 0; }

    /* swizzle the linear BGRA into the NV2A's swizzled A8R8G8B8 layout */
    XGSwizzleRect((void*)bgra, w * 4, NULL, lr.pBits, w, h, NULL, 4);
    tex->UnlockRect(0);

    /* swap in only now that we've succeeded */
    if (s_atlas_tex) { s_atlas_tex->Release(); s_atlas_tex = NULL; }
    s_atlas_tex = tex;
    s_atlasW = w; s_atlasH = h;
    return 1;
}

/* Load the baked "Default" font (the always-present failsafe). */
static int Font_LoadDefault(IDirect3DDevice8* pDevice) {
    s_metrics[0] = g_glyphsSmall;
    s_metrics[1] = g_glyphsMedium;
    s_metrics[2] = g_glyphsLarge;
    if (!BuildAtlasTexture(pDevice, g_fontAtlasData,
        FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT))
        return 0;
    s_layoutScale[0] = s_layoutScale[1] = s_layoutScale[2] = 1.0f;
    s_usingCustom = 0;
    RecomputeMetrics();
    return 1;
}

int Font_Init(IDirect3DDevice8* pDevice) {
    if (s_atlas_tex) return 1; /* already initialised */
    return Font_LoadDefault(pDevice);
}

/* Load a custom .ddf from disk. On any failure the current font is untouched
   (so callers can fall back to / stay on Default). Returns 1 on success.

   .ddf layout (little-endian): see tools/ddf_encoder.py
     u32 magic 'DDF1', u32 ver, u32 atlasW, u32 atlasH, u32 sizePx[3],
     then 3*95 GlyphMetrics (6*i32 each), then atlasW*atlasH*4 BGRA bytes. */
int Font_LoadDDF(IDirect3DDevice8* pDevice, const char* path) {
    HANDLE h;
    DWORD  got = 0, hdr[7];
    int    aw, ah, sz, i;
    unsigned char* pixels = NULL;
    DWORD  pxBytes;
    int    ok = 0;

    if (!pDevice || !path || !path[0]) return 0;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    /* header: 7 u32 = magic,ver,aw,ah,s0,s1,s2 */
    if (!ReadFile(h, hdr, sizeof(hdr), &got, NULL) || got != sizeof(hdr)) goto done;
    if (hdr[0] != 0x31464444u || hdr[1] != 1u) goto done;   /* 'DDF1' v1 */
    aw = (int)hdr[2]; ah = (int)hdr[3];
    if (aw < 16 || ah < 16 || aw > 4096 || ah > 4096) goto done;

    /* metrics: 3 sizes x 95 glyphs x 6 i32 -- read straight into temp arrays */
    for (sz = 0; sz < 3; sz++) {
        for (i = 0; i < 95; i++) {
            int rec[6];
            if (!ReadFile(h, rec, sizeof(rec), &got, NULL) || got != sizeof(rec)) goto done;
            s_loadedMetrics[sz][i].x = rec[0];
            s_loadedMetrics[sz][i].y = rec[1];
            s_loadedMetrics[sz][i].w = rec[2];
            s_loadedMetrics[sz][i].h = rec[3];
            s_loadedMetrics[sz][i].advance = rec[4];
            s_loadedMetrics[sz][i].bear_y = rec[5];
        }
    }

    /* atlas pixels */
    pxBytes = (DWORD)aw * (DWORD)ah * 4;
    pixels = (unsigned char*)malloc(pxBytes);
    if (!pixels) goto done;
    if (!ReadFile(h, pixels, pxBytes, &got, NULL) || got != pxBytes) goto done;

    /* build the texture (swaps in only on success) */
    if (!BuildAtlasTexture(pDevice, pixels, aw, ah)) goto done;

    /* point active metrics at the loaded set */
    s_metrics[0] = s_loadedMetrics[0];
    s_metrics[1] = s_loadedMetrics[1];
    s_metrics[2] = s_loadedMetrics[2];

    /* layout scale = virtual_size / raster_size for each size, so the hi-res
       (720p-rasterized) glyphs occupy the same on-screen footprint as Default
       and don't overflow / clip the frames. */
    for (sz = 0; sz < 3; sz++) {
        int raster = (int)hdr[4 + sz];     /* sizePx[sz] from the header */
        s_layoutScale[sz] = (raster > 0)
            ? ((float)k_virtSize[sz] / (float)raster) : 1.0f;
    }

    s_usingCustom = 1;
    RecomputeMetrics();
    ok = 1;

done:
    if (pixels) free(pixels);
    CloseHandle(h);
    return ok;
}

/* Revert to the baked Default font (used when a custom font is deselected or
   fails). Safe to call any time after Font_Init. */
int Font_UseDefault(IDirect3DDevice8* pDevice) {
    return Font_LoadDefault(pDevice);
}

int Font_IsCustom(void) { return s_usingCustom; }

void Font_Shutdown(void) {
    if (s_atlas_tex) { s_atlas_tex->Release(); s_atlas_tex = NULL; }
}

/*    Measurement                                                              */

int Font_MeasureText(const char* str, int size) {
    const GlyphMetrics* metrics;
    float width = 0.0f;
    float scale;
    unsigned char ch;

    if (!str || size < 0 || size > 2) return 0;
    metrics = s_metrics[size];
    scale = s_layoutScale[size];

    while ((ch = (unsigned char)*str++) != 0) {
        if (ch < 32 || ch > 126) continue;
        width += (float)metrics[ch - 32].advance * scale;
    }
    return (int)width;
}

int Font_GlyphHeight(int size) {
    if (size < 0 || size > 2) return 16;
    return s_glyph_h[size];
}

/* Recommended row pitch: the full glyph box plus a little leading, so adjacent
   rows don't touch and a row's text never grazes the slot below it. This is the
   value list screens should use for row spacing (rather than a hardcoded pitch
   tuned to one font). */
int Font_LineHeight(int size) {
    int gh;
    if (size < 0 || size > 2) return 18;
    gh = s_glyph_h[size];
    return gh + (gh / 6) + 2;        /* ~17% leading + 2px floor */
}

/*    Drawing                                                                  */

void Font_DrawText(IDirect3DDevice8* pDevice,
    float x, float y,
    const char* str,
    int size,
    DWORD colour,
    int max_w) {
    const GlyphMetrics* metrics;
    FontVert verts[4];
    float sx = UI_Sx(x);
    float sy = UI_Sy(y);
    float cur_x = sx;
    float end_x = (max_w > 0) ? (sx + UI_ScaleX((float)max_w)) : 1e9f;
    float scale;
    unsigned char ch;
    const GlyphMetrics* gm;
    float u0, v0, u1, v1;
    float gx, gy, gw, gh;

    if (!s_atlas_tex || !str || size < 0 || size > 2) return;
    metrics = s_metrics[size];
    scale = s_layoutScale[size];

    /* Set render states for alpha-blended text */
    pDevice->SetTexture(0, s_atlas_tex);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    /* Point filter + clamp address -- Xbox NV2A values from D3D8Types.h */
    pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
    pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
    pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    pDevice->SetVertexShader(FONT_FVF);

    while ((ch = (unsigned char)*str++) != 0) {
        if (ch < 32 || ch > 126) continue;
        gm = &metrics[ch - 32];
        if (gm->w == 0) { cur_x += UI_ScaleX((float)gm->advance * scale); continue; }
        if (max_w > 0 && cur_x + UI_ScaleX((float)gm->w * scale) > end_x) break;

        /* UV coordinates use raw atlas pixels (texture source, never scaled) */
        u0 = (float)gm->x / ATLAS_W;
        v0 = (float)gm->y / ATLAS_H;
        u1 = (float)(gm->x + gm->w) / ATLAS_W;
        v1 = (float)(gm->y + gm->h) / ATLAS_H;

        gx = cur_x;
        /* on-screen size + bearing scaled to virtual units (layout footprint),
           so a hi-res custom font occupies the same space as Default */
        gy = sy + UI_ScaleY((float)s_max_ascender[size] + (float)gm->bear_y * scale);
        gw = UI_ScaleX((float)gm->w * scale);
        gh = UI_ScaleY((float)gm->h * scale);

        /* Two triangles as a quad (screen-space, XYZRHW) */
        verts[0].x = gx;    verts[0].y = gy;    verts[0].z = 0; verts[0].rhw = 1; verts[0].colour = colour; verts[0].u = u0; verts[0].v = v0;
        verts[1].x = gx + gw; verts[1].y = gy;    verts[1].z = 0; verts[1].rhw = 1; verts[1].colour = colour; verts[1].u = u1; verts[1].v = v0;
        verts[2].x = gx;    verts[2].y = gy + gh; verts[2].z = 0; verts[2].rhw = 1; verts[2].colour = colour; verts[2].u = u0; verts[2].v = v1;
        verts[3].x = gx + gw; verts[3].y = gy + gh; verts[3].z = 0; verts[3].rhw = 1; verts[3].colour = colour; verts[3].u = u1; verts[3].v = v1;

        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(FontVert));

        cur_x += UI_ScaleX((float)gm->advance * scale);
    }

    /* Restore states */
    pDevice->SetTexture(0, NULL);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
}

void Font_DrawTextCentered(IDirect3DDevice8* pDevice,
    float cx, float y,
    float width,
    const char* str,
    int size,
    DWORD colour) {
    int tw = Font_MeasureText(str, size);
    float x = cx + (width - tw) * 0.5f;
    Font_DrawText(pDevice, x, y, str, size, colour, 0);
}

void Font_DrawTextRight(IDirect3DDevice8* pDevice,
    float x, float y,
    const char* str,
    int size,
    DWORD colour) {
    int tw = Font_MeasureText(str, size);
    Font_DrawText(pDevice, x - tw, y, str, size, colour, 0);
}
/*    Iso-plane text -- glyph quads in world space, tilts with the chrome.    */

void Font_DrawTextIso(IDirect3DDevice8* pDevice,
    float vx, float vy, const char* str, int size, DWORD colour) {
    const GlyphMetrics* metrics;
    FontIsoVert verts[4];
    float cx = UI_VIRT_W * 0.5f;
    float cy = UI_VIRT_H * 0.5f;
    float cur_x = vx;
    float scale;
    unsigned char ch;
    const GlyphMetrics* gm;
    float u0, v0, u1, v1;
    float gx, gy, gw, gh;
    float wx0, wx1, wy0, wy1;

    if (!s_atlas_tex || !str || size < 0 || size > 2) return;
    metrics = s_metrics[size];
    scale = s_layoutScale[size];

    /* Relies on the iso WORLD/VIEW/PROJECTION already set by Iso_Begin(). */
    pDevice->SetTexture(0, s_atlas_tex);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    /* linear keeps the glyphs from shimmering on the tilt */
    pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    pDevice->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    while ((ch = (unsigned char)*str++) != 0) {
        if (ch < 32 || ch > 126) continue;
        gm = &metrics[ch - 32];
        if (gm->w == 0) { cur_x += (float)gm->advance * scale; continue; }

        u0 = (float)gm->x / ATLAS_W;
        v0 = (float)gm->y / ATLAS_H;
        u1 = (float)(gm->x + gm->w) / ATLAS_W;
        v1 = (float)(gm->y + gm->h) / ATLAS_H;

        gx = cur_x;
        gy = vy + (float)s_max_ascender[size] + (float)gm->bear_y * scale;
        gw = (float)gm->w * scale;
        gh = (float)gm->h * scale;

        /* virtual -> centred world XY (flip Y so up is +Y), z = 0 */
        wx0 = gx - cx;        wx1 = gx + gw - cx;
        wy0 = cy - gy;        wy1 = cy - (gy + gh);

        verts[0].x = wx0; verts[0].y = wy0; verts[0].z = 0; verts[0].colour = colour; verts[0].u = u0; verts[0].v = v0;
        verts[1].x = wx1; verts[1].y = wy0; verts[1].z = 0; verts[1].colour = colour; verts[1].u = u1; verts[1].v = v0;
        verts[2].x = wx0; verts[2].y = wy1; verts[2].z = 0; verts[2].colour = colour; verts[2].u = u0; verts[2].v = v1;
        verts[3].x = wx1; verts[3].y = wy1; verts[3].z = 0; verts[3].colour = colour; verts[3].u = u1; verts[3].v = v1;

        pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(FontIsoVert));
        cur_x += (float)gm->advance * scale;
    }

    pDevice->SetTexture(0, NULL);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}