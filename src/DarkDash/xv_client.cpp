/*===========================================================================
    xv_client.cpp -- X-View protocol client for the Xbox
                     Team Resurgent / Darkone83

    Builds X-View packets with EXPLICIT byte offsets (little-endian) and pushes
    them through xv_xbox's bulk transport. This is the byte-for-byte mirror of
    the Python reference client; the firmware's xv_proto.c parses exactly these
    bytes.

    Header (8 bytes): magic(u16) opcode(u8) flags(u8) length(u16) seq(u16)
===========================================================================*/

#include <xtl.h>
#include "xv_client.h"
#include "xv_xbox.h"
#include "xv_protocol.h"

/* host-incrementing sequence; echoed by the device in replies */
static unsigned short s_seq = 1;

/* ---- little-endian writers/readers (no struct packing assumptions) ---- */
static void wr16(unsigned char* p, unsigned int v) { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); }
static unsigned int rd16(const unsigned char* p) { return (unsigned int)p[0] | ((unsigned int)p[1] << 8); }
static unsigned int rd32(const unsigned char* p) { return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24); }

uint16_t XvCli_Rgb565(int r, int g, int b)
{
    /* This panel is wired BGR: a value in the high 5 bits displays as blue. So
       emit BGR565 -- blue in the high bits, red in the low bits. Every color
       path (fill/text/clear/bars and the plasma palette) goes through here, so
       this one swap corrects them all. */
    return (uint16_t)(((b & 0xF8) << 8) | ((g & 0xFC) << 3) | ((r & 0xF8) >> 3));
}

/* Build header into buf[0..7]; returns 8. */
static int put_header(unsigned char* buf, unsigned char opcode, unsigned int length, unsigned short seq)
{
    wr16(buf + 0, XV_PROTO_MAGIC);
    buf[2] = opcode;
    buf[3] = 0;                  /* flags */
    wr16(buf + 4, length);
    wr16(buf + 6, seq);
    return 8;
}

/* Send a command (header + optional payload already laid out after byte 8).
   total = 8 + payload_len. Returns 0 on success. */
static int send_cmd(unsigned char* buf, int total)
{
    int n = XvXbox_SendBulk(buf, total);
    return (n == total) ? 0 : -1;
}

/* Send a header-only (no payload) command. */
static int send_simple(unsigned char opcode)
{
    unsigned char buf[8];
    put_header(buf, opcode, 0, s_seq++);
    return send_cmd(buf, 8);
}

/* Send a request, then read + validate a reply of `want` opcode.
   Copies up to copy_len payload bytes into out_payload (may be 0). Returns 0 ok. */
static int request_reply(unsigned char req_op, unsigned char want_op,
    unsigned char* out_payload, int copy_len)
{
    unsigned char hdr[8];
    unsigned char rx[128];
    unsigned short use_seq = s_seq++;
    unsigned int magic, length;
    int got, i;

    put_header(hdr, req_op, 0, use_seq);
    if (send_cmd(hdr, 8) != 0) return -1;

    got = XvXbox_RecvBulk(rx, sizeof(rx));
    if (got < 8) return -2;

    magic = rd16(rx + 0);
    length = rd16(rx + 4);
    if (magic != XV_PROTO_MAGIC) return -3;
    if (rx[2] != want_op)        return -4;

    if (out_payload && copy_len > 0) {
        int n = (int)length;
        if (n > copy_len) n = copy_len;
        if (n > (got - 8)) n = (got - 8);
        for (i = 0; i < n; i++) out_payload[i] = rx[8 + i];
    }
    return 0;
}

/*---------------------------------------------------------------------------
    Handshake
---------------------------------------------------------------------------*/
int XvCli_Ping(void)
{
    return request_reply(XV_OP_PING, XV_OP_PONG, 0, 0);
}

int XvCli_QueryInfo(XvInfo* out)
{
    unsigned char p[28];
    int i, rc;
    for (i = 0; i < 28; i++) p[i] = 0;
    rc = request_reply(XV_OP_QUERY_INFO, XV_OP_INFO, p, 28);
    if (rc != 0) return rc;
    if (out) {
        out->proto_version = (int)rd16(p + 0);
        out->fw_version = (int)rd16(p + 2);
        out->width = (int)rd16(p + 4);
        out->height = (int)rd16(p + 6);
        out->cols = (int)rd16(p + 8);
        out->rows = (int)rd16(p + 10);
        out->font_w = (int)p[12];
        out->font_h = (int)p[13];
        out->color_format = (int)p[14];
        out->orientation = (int)p[15];
        out->caps = (int)p[16];
        /* p[17] = _pad */
        out->max_payload = (int)rd16(p + 18);
        out->glyph_cache_bytes = rd32(p + 20);
        out->sprite_cache_bytes = rd32(p + 24);
    }
    return 0;
}

int XvCli_GetStatus(XvStatus* out)
{
    unsigned char p[6];
    int i, rc;
    for (i = 0; i < 6; i++) p[i] = 0;
    rc = request_reply(XV_OP_STATUS_QUERY, XV_OP_STATUS, p, 6);
    if (rc != 0) return rc;
    if (out) {
        out->last_seq = (int)rd16(p + 0);
        out->flags = (int)p[2];
        out->last_error = (int)p[3];
        out->dropped = (int)rd16(p + 4);
    }
    return 0;
}

/*---------------------------------------------------------------------------
    Display config (fire-and-forget)
---------------------------------------------------------------------------*/
void XvCli_SetBrightness(int duty)
{
    unsigned char buf[9];
    put_header(buf, XV_OP_SET_BRIGHTNESS, 1, s_seq++);
    buf[8] = (unsigned char)(duty & 0xFF);
    send_cmd(buf, 9);
}

void XvCli_SetPanel(int panel)
{
    unsigned char buf[9];
    put_header(buf, XV_OP_SET_PANEL, 1, s_seq++);
    buf[8] = (unsigned char)(panel & 0xFF);
    send_cmd(buf, 9);
}
void XvCli_Power(int mode)
{
    unsigned char buf[9];
    put_header(buf, XV_OP_POWER, 1, s_seq++);
    buf[8] = (unsigned char)(mode & 0xFF);
    send_cmd(buf, 9);
}
void XvCli_SetPresentMode(int mode)
{
    unsigned char buf[9];
    put_header(buf, XV_OP_SET_PRESENT_MODE, 1, s_seq++);
    buf[8] = (unsigned char)(mode & 0xFF);
    send_cmd(buf, 9);
}
void XvCli_Reset(void)
{
    send_simple(XV_OP_RESET);
}

/*---------------------------------------------------------------------------
    Graphics (fire-and-forget)
---------------------------------------------------------------------------*/
void XvCli_Clear(uint16_t color)
{
    unsigned char buf[10];
    put_header(buf, XV_OP_CLEAR, 2, s_seq++);
    wr16(buf + 8, color);
    send_cmd(buf, 10);
}
void XvCli_FillRect(int x, int y, int w, int h, uint16_t color)
{
    unsigned char buf[18];                 /* 8 + xv_fill_rect_t(10) */
    put_header(buf, XV_OP_FILL_RECT, 10, s_seq++);
    wr16(buf + 8, (unsigned int)x);
    wr16(buf + 10, (unsigned int)y);
    wr16(buf + 12, (unsigned int)w);
    wr16(buf + 14, (unsigned int)h);
    wr16(buf + 16, color);
    send_cmd(buf, 18);
}
void XvCli_Present(void)
{
    /* length 0 = full flush */
    send_simple(XV_OP_PRESENT);
}
void XvCli_PresentRect(int x, int y, int w, int h)
{
    unsigned char buf[16];                 /* 8 + xv_rect_t(8) */
    put_header(buf, XV_OP_PRESENT, 8, s_seq++);
    wr16(buf + 8, (unsigned int)x);
    wr16(buf + 10, (unsigned int)y);
    wr16(buf + 12, (unsigned int)w);
    wr16(buf + 14, (unsigned int)h);
    send_cmd(buf, 16);
}

/*---------------------------------------------------------------------------
    Pixel blit (BLIT_RECT) -- streamed in small chunks.

    The OG Xbox USB stack will not push a single multi-KB bulk URB reliably
    (the largest proven transfer is the ~271-byte TEXT command). But the
    firmware's parser is a byte stream -- it does not care about transfer
    boundaries -- so we send each band's header+pixels as many small transfers.
    XV_BLIT_CHUNK is deliberately conservative (== the firmware vendor FIFO, so
    it cannot overrun); raise it once the path is confirmed to claw back speed.
    Returns the number of failed transfers (0 = clean).
---------------------------------------------------------------------------*/
#define XV_BLIT_CHUNK 4096   /* host URB size; firmware drains a byte-stream so this only cuts per-URB overhead (was 512 == FIFO) */

/* Internal: stream a BLIT_RECT (optionally scaled). `scale`>1 sets the scale
   nibble in the header flags so the firmware nearest-upscales the source. The
   blit rect carries the SOURCE w,h; dest covers scale*w x scale*h at (x,y).
   Returns failed-transfer count (0 = clean). */
static int blit_core(int x, int y, int w, int h, int scale, const uint16_t* px)
{
    int maxBandRows, row, fails = 0;
    unsigned char flags = (scale > 1) ? (unsigned char)XV_HF_MAKE_SCALE(scale) : 0;
    if (!px || w <= 0 || h <= 0) return -1;
    if (scale < 1) scale = 1;

    /* band height capped so 8 + w*bh*2 stays within the u16 length field */
    maxBandRows = 65527 / (w * 2);
    if (maxBandRows < 1) maxBandRows = 1;

    for (row = 0; row < h; row += maxBandRows) {
        unsigned char hdr[16];
        int bh = h - row; if (bh > maxBandRows) bh = maxBandRows;
        {
            int pxbytes = w * bh * 2;
            const unsigned char* pb = (const unsigned char*)(px + (row * w));
            int sent = 0;

            /* command prefix: 8-byte xfer header (scale in flags) + blit rect.
               dest y advances by scale per source row band. */
            put_header(hdr, XV_OP_BLIT_RECT, (unsigned int)(8 + pxbytes), s_seq++);
            hdr[3] = flags;
            wr16(hdr + 8, (unsigned int)x);
            wr16(hdr + 10, (unsigned int)(y + row * scale));
            wr16(hdr + 12, (unsigned int)w);
            wr16(hdr + 14, (unsigned int)bh);
            if (XvXbox_SendBulk(hdr, 16) != 16) fails++;

            while (sent < pxbytes) {
                int c = pxbytes - sent; if (c > XV_BLIT_CHUNK) c = XV_BLIT_CHUNK;
                if (XvXbox_SendBulk(pb + sent, c) != c) fails++;
                sent += c;
            }
        }
    }
    return fails;
}

int XvCli_Blit(int x, int y, int w, int h, const uint16_t* px)
{
    return blit_core(x, y, w, h, 1, px);
}

int XvCli_BlitScaled(int x, int y, int w, int h, int scale, const uint16_t* px)
{
    return blit_core(x, y, w, h, scale, px);
}
void XvCli_TextPut(int row, int col, uint16_t fg, uint16_t bg, const char* s)
{
    unsigned char buf[8 + 8 + 255];        /* header + xv_text_put_t(8) + chars */
    int count = 0, i, total;
    if (s) { while (s[count] && count < 255) count++; }

    put_header(buf, XV_OP_TEXT_PUT, (unsigned int)(8 + count), s_seq++);
    buf[8] = (unsigned char)row;
    buf[9] = (unsigned char)col;
    wr16(buf + 10, fg);
    wr16(buf + 12, bg);
    buf[14] = (unsigned char)count;
    buf[15] = 0;                           /* _pad */
    for (i = 0; i < count; i++) buf[16 + i] = (unsigned char)s[i];

    total = 8 + 8 + count;
    send_cmd(buf, total);
}
void XvCli_TextClearAll(uint16_t bg)
{
    /* length 0 = clear all (bg arg ignored by firmware's all-clear path) */
    (void)bg;
    send_simple(XV_OP_TEXT_CLEAR);
}