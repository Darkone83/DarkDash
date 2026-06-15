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

#ifdef __cplusplus
}
#endif

#endif /* XV_CLIENT_H */