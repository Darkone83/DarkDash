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

/*===========================================================================
    v2 additions (additive; mirrors python/xv_client.py). Builds the new
    co-processor opcodes with the same explicit-offset packing as v1. Colors
    still flow through XvCli_Rgb565 so there remains exactly ONE channel swap.
===========================================================================*/

static void wr32(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Chunked sender for the larger v2 buffered packets (command lists, meshes).
   The device parser is transfer-boundary agnostic, so a packet may span several
   bulk OUTs. 512 == the doc-recommended ceiling for the Xbox stack on the new
   streaming paths; blits keep their own (proven) XV_BLIT_CHUNK. */
#define XV_CMD_CHUNK 512
static int send_cmd_chunked(unsigned char* buf, int total)
{
    int sent = 0, c;
    while (sent < total) {
        c = total - sent; if (c > XV_CMD_CHUNK) c = XV_CMD_CHUNK;
        if (XvXbox_SendBulk(buf + sent, c) != c) return -1;
        sent += c;
    }
    return 0;
}

/* request with payload, then read + validate a reply (Sync needs this). */
static int request_reply_p(unsigned char req_op, const unsigned char* payload, int plen,
    unsigned char want_op, unsigned char* out, int copy_len)
{
    unsigned char buf[72];
    unsigned char rx[128];
    unsigned short use_seq = s_seq++;
    unsigned int magic, length;
    int got, i, n;
    if (plen < 0 || plen > 64) return -1;
    put_header(buf, req_op, (unsigned int)plen, use_seq);
    for (i = 0; i < plen; i++) buf[8 + i] = payload[i];
    if (send_cmd(buf, 8 + plen) != 0) return -1;
    got = XvXbox_RecvBulk(rx, sizeof(rx));
    if (got < 8) return -2;
    magic = rd16(rx + 0); length = rd16(rx + 4);
    if (magic != XV_PROTO_MAGIC) return -3;
    if (rx[2] != want_op)        return -4;
    if (out && copy_len > 0) {
        n = (int)length; if (n > copy_len) n = copy_len; if (n > got - 8) n = got - 8;
        for (i = 0; i < n; i++) out[i] = rx[8 + i];
    }
    return 0;
}

/* ----------------------------- control: caps / heartbeat / sync ---------- */
int XvCli_QueryCaps(XvCaps* out)
{
    unsigned char rx[32];
    int rc;
    if (!out) return -1;
    rc = request_reply(XV_OP_QUERY_CAPS, XV_OP_CAPS, rx, 32);
    if (rc != 0) return rc;
    out->proto_version = (int)rd16(rx + 0);
    out->struct_bytes = (int)rd16(rx + 2);
    out->caps32 = rd32(rx + 4);
    out->tile_w = (int)rd16(rx + 8);
    out->tile_h = (int)rd16(rx + 10);
    out->max_layers = (int)rd16(rx + 12);
    out->max_meshes = (int)rd16(rx + 14);
    out->max_verts = (int)rd16(rx + 16);
    out->max_scenes = (int)rd16(rx + 18);
    out->max_anims = (int)rd16(rx + 20);
    out->max_assets = (int)rd16(rx + 22);
    out->ram_asset_bytes = rd32(rx + 24);
    out->flash_asset_bytes = rd32(rx + 28);
    return 0;
}

void XvCli_Heartbeat(void) { send_simple(XV_OP_HEARTBEAT); }

int XvCli_Sync(unsigned int epoch, XvSync* out)
{
    unsigned char pl[12], rx[12];
    int rc, i;
    for (i = 0; i < 12; i++) pl[i] = 0;
    wr32(pl + 0, epoch);                 /* host's last-known epoch; rest 0 */
    rc = request_reply_p(XV_OP_SYNC, pl, 12, XV_OP_SYNC_ACK, rx, 12);
    if (rc != 0) return rc;
    if (out) {
        out->epoch = rd32(rx + 0);
        out->scene_active = (int)rd16(rx + 4);
        out->assets_held = (int)rd16(rx + 6);
        out->scenes_held = (int)rd16(rx + 8);
    }
    return 0;
}

/* ----------------------------- command-buffer builder -------------------- */
/* Append one sub-command (sub_op, arg_bytes, args) into the list body, which
   lives at buf+8 (8 bytes reserved for the packet header). Silently drops on
   overflow so a builder can never run past its storage. */
static void cmd_put(XvCmd* c, unsigned char op, const unsigned char* a, int n)
{
    unsigned char* p; int i;
    if (!c || !c->buf) return;
    if (8 + c->len + 2 + n > c->cap) return;
    p = c->buf + 8 + c->len;
    p[0] = op; p[1] = (unsigned char)n;
    for (i = 0; i < n; i++) p[2 + i] = a[i];
    c->len += 2 + n;
}

void XvCmd_Begin(XvCmd* c, unsigned char* storage, int cap)
{
    if (!c) return; c->buf = storage; c->cap = cap; c->len = 0;
}

void XvCmd_SetColor(XvCmd* c, uint16_t rgb)
{
    unsigned char a[2]; wr16(a, rgb); cmd_put(c, XV_CMD_SET_COLOR, a, 2);
}

void XvCmd_SetClip(XvCmd* c, int x, int y, int w, int h)
{
    unsigned char a[8]; wr16(a + 0, (unsigned)x); wr16(a + 2, (unsigned)y); wr16(a + 4, (unsigned)w); wr16(a + 6, (unsigned)h);
    cmd_put(c, XV_CMD_SET_CLIP, a, 8);
}

void XvCmd_SetBlend(XvCmd* c, int mode)
{
    unsigned char a[1]; a[0] = (unsigned char)mode; cmd_put(c, XV_CMD_SET_BLEND, a, 1);
}

void XvCmd_Fill(XvCmd* c, int x, int y, int w, int h)
{
    unsigned char a[8]; wr16(a + 0, (unsigned)x); wr16(a + 2, (unsigned)y); wr16(a + 4, (unsigned)w); wr16(a + 6, (unsigned)h);
    cmd_put(c, XV_CMD_FILL, a, 8);
}

void XvCmd_Rect(XvCmd* c, int x, int y, int w, int h)
{
    unsigned char a[8]; wr16(a + 0, (unsigned)x); wr16(a + 2, (unsigned)y); wr16(a + 4, (unsigned)w); wr16(a + 6, (unsigned)h);
    cmd_put(c, XV_CMD_RECT, a, 8);
}

void XvCmd_Line(XvCmd* c, int x0, int y0, int x1, int y1)
{
    unsigned char a[8]; wr16(a + 0, (unsigned)x0); wr16(a + 2, (unsigned)y0); wr16(a + 4, (unsigned)x1); wr16(a + 6, (unsigned)y1);
    cmd_put(c, XV_CMD_LINE, a, 8);
}

void XvCmd_HLine(XvCmd* c, int x, int y, int w)
{
    unsigned char a[6]; wr16(a + 0, (unsigned)x); wr16(a + 2, (unsigned)y); wr16(a + 4, (unsigned)w); cmd_put(c, XV_CMD_HLINE, a, 6);
}

void XvCmd_VLine(XvCmd* c, int x, int y, int h)
{
    unsigned char a[6]; wr16(a + 0, (unsigned)x); wr16(a + 2, (unsigned)y); wr16(a + 4, (unsigned)h); cmd_put(c, XV_CMD_VLINE, a, 6);
}

void XvCmd_Pixel(XvCmd* c, int x, int y)
{
    unsigned char a[4]; wr16(a + 0, (unsigned)x); wr16(a + 2, (unsigned)y); cmd_put(c, XV_CMD_PIXEL, a, 4);
}

void XvCmd_Sprite(XvCmd* c, int id, int x, int y)
{
    unsigned char a[6]; wr16(a + 0, (unsigned)id); wr16(a + 2, (unsigned)x); wr16(a + 4, (unsigned)y); cmd_put(c, XV_CMD_SPRITE, a, 6);
}

void XvCmd_Text(XvCmd* c, int row, int col, const char* s)
{
    unsigned char a[3 + 255];
    int n = 0;
    if (s) { while (s[n] && n < 255) n++; }
    a[0] = (unsigned char)row; a[1] = (unsigned char)col; a[2] = (unsigned char)n;
    { int i; for (i = 0; i < n; i++) a[3 + i] = (unsigned char)s[i]; }
    cmd_put(c, XV_CMD_TEXT, a, 3 + n);
}

void XvCmd_Mesh(XvCmd* c, int meshId)
{
    unsigned char a[2]; wr16(a, (unsigned)meshId); cmd_put(c, XV_CMD_MESH, a, 2);
}

void XvCmd_Gradient(XvCmd* c, int x, int y, int w, int h, uint16_t c0, uint16_t c1, int dir)
{
    unsigned char a[13];
    wr16(a + 0, (unsigned)x); wr16(a + 2, (unsigned)y); wr16(a + 4, (unsigned)w); wr16(a + 6, (unsigned)h);
    wr16(a + 8, c0); wr16(a + 10, c1); a[12] = (unsigned char)dir; cmd_put(c, XV_CMD_GRADIENT, a, 13);
}

void XvCmd_Present(XvCmd* c) { cmd_put(c, XV_CMD_PRESENT, 0, 0); }

int XvCli_CmdList(XvCmd* c)
{
    if (!c || !c->buf) return -1;
    put_header(c->buf, XV_OP_CMD_LIST, (unsigned int)c->len, s_seq++);
    return send_cmd_chunked(c->buf, 8 + c->len);
}

/* ----------------------------- geometry ---------------------------------- */
int XvCli_DefineMesh(int id, const XvVtx* v, int nV, const uint16_t* idx, int nI, int mode, int flags)
{
    unsigned char buf[8 + 2048];
    int payload, i, off;
    if (nV < 0 || nI < 0) return -1;
    payload = 8 + nV * 16 + nI * 2;       /* mesh header + verts + indices */
    if (payload > 2048) return -1;        /* BUF_MAX on the device */
    put_header(buf, XV_OP_DEFINE_MESH, (unsigned int)payload, s_seq++);
    wr16(buf + 8, (unsigned)id);
    wr16(buf + 10, (unsigned)nV);
    wr16(buf + 12, (unsigned)nI);
    buf[14] = (unsigned char)mode;
    buf[15] = (unsigned char)flags;
    off = 16;
    for (i = 0; i < nV; i++) {
        wr32(buf + off + 0, (unsigned int)v[i].x);
        wr32(buf + off + 4, (unsigned int)v[i].y);
        wr32(buf + off + 8, (unsigned int)v[i].z);
        wr16(buf + off + 12, v[i].color);
        wr16(buf + off + 14, 0);
        off += 16;
    }
    for (i = 0; i < nI; i++) { wr16(buf + off, idx[i]); off += 2; }
    return send_cmd_chunked(buf, 8 + payload);
}

void XvCli_SetMatrix(const int32_t m[12])
{
    unsigned char buf[8 + 48];
    int i;
    put_header(buf, XV_OP_SET_MATRIX, 48, s_seq++);
    for (i = 0; i < 12; i++) wr32(buf + 8 + i * 4, (unsigned int)m[i]);
    send_cmd(buf, 8 + 48);
}

void XvCli_DrawMesh(int id, int flags)
{
    unsigned char buf[8 + 4];
    put_header(buf, XV_OP_DRAW_MESH, 4, s_seq++);
    wr16(buf + 8, (unsigned)id);
    buf[10] = (unsigned char)flags;
    buf[11] = 0;
    send_cmd(buf, 8 + 4);
}

void XvCli_FreeMesh(int id)
{
    unsigned char buf[8 + 2];
    put_header(buf, XV_OP_FREE_MESH, 2, s_seq++);
    wr16(buf + 8, (unsigned)id);
    send_cmd(buf, 8 + 2);
}

/* ----------------------------- fixed-point 16.16 trig + matrices --------- */
/* quarter-wave sine table, 0..256 maps to 0..90deg, value in 16.16 */
static const int32_t s_sinq[257] = {
         0,    402,    804,   1206,   1608,   2010,   2412,   2814,
      3216,   3617,   4019,   4420,   4821,   5222,   5623,   6023,
      6424,   6824,   7224,   7623,   8022,   8421,   8820,   9218,
      9616,  10014,  10411,  10808,  11204,  11600,  11996,  12391,
     12785,  13180,  13573,  13966,  14359,  14751,  15143,  15534,
     15924,  16314,  16703,  17091,  17479,  17867,  18253,  18639,
     19024,  19409,  19792,  20175,  20557,  20939,  21320,  21699,
     22078,  22457,  22834,  23210,  23586,  23961,  24335,  24708,
     25080,  25451,  25821,  26190,  26558,  26925,  27291,  27656,
     28020,  28383,  28745,  29106,  29466,  29824,  30182,  30538,
     30893,  31248,  31600,  31952,  32303,  32652,  33000,  33347,
     33692,  34037,  34380,  34721,  35062,  35401,  35738,  36075,
     36410,  36744,  37076,  37407,  37736,  38064,  38391,  38716,
     39040,  39362,  39683,  40002,  40320,  40636,  40951,  41264,
     41576,  41886,  42194,  42501,  42806,  43110,  43412,  43713,
     44011,  44308,  44604,  44898,  45190,  45480,  45769,  46056,
     46341,  46624,  46906,  47186,  47464,  47741,  48015,  48288,
     48559,  48828,  49095,  49361,  49624,  49886,  50146,  50404,
     50660,  50914,  51166,  51417,  51665,  51911,  52156,  52398,
     52639,  52878,  53114,  53349,  53581,  53812,  54040,  54267,
     54491,  54714,  54934,  55152,  55368,  55582,  55794,  56004,
     56212,  56418,  56621,  56823,  57022,  57219,  57414,  57607,
     57798,  57986,  58172,  58356,  58538,  58718,  58896,  59071,
     59244,  59415,  59583,  59750,  59914,  60075,  60235,  60392,
     60547,  60700,  60851,  60999,  61145,  61288,  61429,  61568,
     61705,  61839,  61971,  62101,  62228,  62353,  62476,  62596,
     62714,  62830,  62943,  63054,  63162,  63268,  63372,  63473,
     63572,  63668,  63763,  63854,  63944,  64031,  64115,  64197,
     64277,  64354,  64429,  64501,  64571,  64639,  64704,  64766,
     64827,  64884,  64940,  64993,  65043,  65091,  65137,  65180,
     65220,  65259,  65294,  65328,  65358,  65387,  65413,  65436,
     65457,  65476,  65492,  65505,  65516,  65525,  65531,  65535,
     65536,
};

int32_t XvFx_Sin(int ang)
{
    int q, i;
    ang &= (XV_FX_TURN - 1);     /* wrap to 0..1023 */
    q = ang >> 8;                /* quadrant 0..3   */
    i = ang & 255;
    if (q == 0) return  s_sinq[i];
    if (q == 1) return  s_sinq[256 - i];
    if (q == 2) return -s_sinq[i];
    return            -s_sinq[256 - i];
}
int32_t XvFx_Cos(int ang) { return XvFx_Sin(ang + (XV_FX_TURN / 4)); }

static int32_t fxmul(int32_t a, int32_t b) { return (int32_t)(((__int64)a * (__int64)b) >> 16); }

void XvMat_Identity(int32_t m[12])
{
    int i; for (i = 0; i < 12; i++) m[i] = 0;
    m[0] = XV_FX_ONE; m[5] = XV_FX_ONE; m[10] = XV_FX_ONE;
}

void XvMat_RotY(int32_t m[12], int ang)
{
    int32_t c = XvFx_Cos(ang), s = XvFx_Sin(ang);
    int i; for (i = 0; i < 12; i++) m[i] = 0;
    m[0] = c;  m[2] = s;
    m[5] = XV_FX_ONE;
    m[8] = -s; m[10] = c;
}

void XvMat_RotX(int32_t m[12], int ang)
{
    int32_t c = XvFx_Cos(ang), s = XvFx_Sin(ang);
    int i; for (i = 0; i < 12; i++) m[i] = 0;
    m[0] = XV_FX_ONE;
    m[5] = c;  m[6] = -s;
    m[9] = s;  m[10] = c;
}

void XvMat_RotZ(int32_t m[12], int ang)
{
    int32_t c = XvFx_Cos(ang), s = XvFx_Sin(ang);
    int i; for (i = 0; i < 12; i++) m[i] = 0;
    m[0] = c;  m[1] = -s;
    m[4] = s;  m[5] = c;
    m[10] = XV_FX_ONE;
}

void XvMat_Translate(int32_t m[12], int32_t tx, int32_t ty, int32_t tz)
{
    m[3] = tx; m[7] = ty; m[11] = tz;
}

void XvMat_Mul(int32_t out[12], const int32_t a[12], const int32_t b[12])
{
    int32_t t[12];
    int r, c;
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 4; c++) {
            int32_t s = fxmul(a[r * 4 + 0], b[0 * 4 + c]) + fxmul(a[r * 4 + 1], b[1 * 4 + c]) + fxmul(a[r * 4 + 2], b[2 * 4 + c]);
            if (c == 3) s += a[r * 4 + 3];      /* b implicit 4th row = [0 0 0 1] */
            t[r * 4 + c] = s;
        }
    }
    for (r = 0; r < 12; r++) out[r] = t[r];
}