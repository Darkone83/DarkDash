/*
 * xv_client.h -- X-View protocol client for the Xbox.
 * Team Resurgent / Darkone83
 *
 * The high-level API the test harness drives, mirroring the PC reference host's
 * xview/client.py one-method-per-opcode surface. Builds X-View packets and
 * pushes them through the xv_xbox.* raw bulk transport. Draw/config calls are
 * fire-and-forget; ping / query_info / get_status read a reply on the IN pipe.
 *
 * Wire bytes are assembled with explicit offsets (see xv_client.cpp), so they
 * match the firmware byte-for-byte regardless of MSVC struct packing.
 */
#ifndef XV_CLIENT_H
#define XV_CLIENT_H

#include <xtl.h>
#include "xv_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* Parsed INFO reply (fields the harness displays). */
    typedef struct {
        int proto_version;
        int fw_version;
        int width, height;
        int cols, rows;
        int font_w, font_h;
        int color_format;
        int orientation;
        int caps;
        int max_payload;
        unsigned int glyph_cache_bytes;
        unsigned int sprite_cache_bytes;
    } XvInfo;

    typedef struct {
        int last_seq;
        int flags;
        int last_error;
        int dropped;
    } XvStatus;

    /* --- handshake (request/response) --- 0 = ok, <0 = no/!bad reply --- */
    int  XvCli_Ping(void);
    int  XvCli_QueryInfo(XvInfo* out);
    int  XvCli_GetStatus(XvStatus* out);

    /* --- display config (fire-and-forget) --- */
    void XvCli_SetBrightness(int duty0to255);
    void XvCli_Power(int mode);          /* XV_PWR_* */
    void XvCli_SetPresentMode(int mode); /* XV_PRESENT_* */
    void XvCli_SetPanel(int panel);      /* XV_PANEL_A / XV_PANEL_B; re-query INFO after */
    void XvCli_Reset(void);

    /* --- graphics (fire-and-forget) --- */
    void XvCli_Clear(uint16_t color);
    void XvCli_FillRect(int x, int y, int w, int h, uint16_t color);
    void XvCli_Present(void);                           /* full flush */
    void XvCli_PresentRect(int x, int y, int w, int h);

    /* Blit a w*h RGB565 pixel buffer (row-major, host LE) to (x,y). Streams in
       small chunks (the Xbox stack won't push large single URBs). Fire-and-forget;
       call XvCli_Present() to flush. Returns the number of failed transfers (0=ok). */
    int XvCli_Blit(int x, int y, int w, int h, const uint16_t* px);

    /* Upscaled blit: source w*h is nearest-expanded by `scale` on the device, so a
       small source fills scale*w x scale*h at (x,y) for a fraction of the wire
       bytes. Returns failed-transfer count (0 = ok). */
    int XvCli_BlitScaled(int x, int y, int w, int h, int scale, const uint16_t* px);

    /* Blit a solid run is just FillRect; a true pixel blit is added next pass
       (it tiles into <=max_payload bands like the Python client). */

       /* --- text --- */
    void XvCli_TextPut(int row, int col, uint16_t fg, uint16_t bg, const char* s);
    void XvCli_TextClearAll(uint16_t bg);

    /* RGB565 helper (matches XV_RGB565). */
    uint16_t XvCli_Rgb565(int r, int g, int b);

    /*===================== v2 additions (gated by caps) =====================*/

    /* Parsed CAPS (QUERY_CAPS reply). Gate every v2 call on caps32 & XV_CAP_*. */
    typedef struct {
        int proto_version, struct_bytes;
        unsigned int caps32;
        int tile_w, tile_h, max_layers, max_meshes, max_verts, max_scenes, max_anims, max_assets;
        unsigned int ram_asset_bytes, flash_asset_bytes;
    } XvCaps;

    /* Parsed SYNC_ACK. If out.epoch != the epoch you sent, the device cold-started
       and you must re-upload meshes/scenes/RAM assets. */
    typedef struct { unsigned int epoch; int scene_active, assets_held, scenes_held; } XvSync;

    int  XvCli_QueryCaps(XvCaps* out);                  /* QUERY_CAPS -> CAPS; 0 = ok      */
    void XvCli_Heartbeat(void);                         /* fire-and-forget; every 1-2 s    */
    int  XvCli_Sync(unsigned int epoch, XvSync* out);   /* SYNC -> SYNC_ACK; 0 = ok        */

    /* Command-buffer builder: batch many primitives into one CMD_LIST packet
       (far fewer URBs than one-op-per-packet). storage must be >= 8 + body bytes. */
    typedef struct { unsigned char* buf; int cap; int len; } XvCmd;
    void XvCmd_Begin(XvCmd* c, unsigned char* storage, int cap);
    void XvCmd_SetColor(XvCmd* c, uint16_t rgb565);     /* pack via XvCli_Rgb565           */
    void XvCmd_SetClip(XvCmd* c, int x, int y, int w, int h);   /* w<0 clears the clip    */
    void XvCmd_SetBlend(XvCmd* c, int mode);            /* 0 copy / 1 alpha-key / 2 add    */
    void XvCmd_Fill(XvCmd* c, int x, int y, int w, int h);
    void XvCmd_Rect(XvCmd* c, int x, int y, int w, int h);
    void XvCmd_Line(XvCmd* c, int x0, int y0, int x1, int y1);
    void XvCmd_HLine(XvCmd* c, int x, int y, int w);
    void XvCmd_VLine(XvCmd* c, int x, int y, int h);
    void XvCmd_Pixel(XvCmd* c, int x, int y);
    void XvCmd_Sprite(XvCmd* c, int id, int x, int y);
    void XvCmd_Text(XvCmd* c, int row, int col, const char* s);
    void XvCmd_Mesh(XvCmd* c, int meshId);          /* draw a mesh with the cur matrix */
    void XvCmd_Gradient(XvCmd* c, int x, int y, int w, int h, uint16_t c0, uint16_t c1, int dir);
    void XvCmd_Present(XvCmd* c);
    int  XvCli_CmdList(XvCmd* c);                       /* send the built list; 0 = ok     */

    /* Geometry (fixed-point 16.16; do trig host-side, ship the matrix). */
    typedef struct { int32_t x, y, z; uint16_t color; } XvVtx;
    int  XvCli_DefineMesh(int id, const XvVtx* verts, int nVerts,
        const uint16_t* indices, int nIndices, int mode, int flags);
    void XvCli_SetMatrix(const int32_t m[12]);          /* 3x4 row-major, 16.16            */
    void XvCli_DrawMesh(int id, int flags);            /* flags bit0 = backface cull      */
    void XvCli_FreeMesh(int id);

    /* 16.16 fixed-point matrix helpers. Angles are 1/1024 of a turn (XV_FX_TURN).
       NB: meshes centered on the origin sit at tz~0 and get near-culled -- push +z
       with XvMat_Translate(m,0,0, 2*XV_FX_ONE .. 3*XV_FX_ONE). */
#define XV_FX_ONE   0x10000
#define XV_FX_TURN  1024
    int32_t XvFx_Sin(int ang);
    int32_t XvFx_Cos(int ang);
    void XvMat_Identity(int32_t m[12]);
    void XvMat_RotX(int32_t m[12], int ang);
    void XvMat_RotY(int32_t m[12], int ang);
    void XvMat_RotZ(int32_t m[12], int ang);
    void XvMat_Translate(int32_t m[12], int32_t tx, int32_t ty, int32_t tz);
    void XvMat_Mul(int32_t out[12], const int32_t a[12], const int32_t b[12]);

#ifdef __cplusplus
}
#endif

#endif /* XV_CLIENT_H */