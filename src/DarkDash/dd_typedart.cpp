/*---------------------------------------------------------------------------
    dd_typedart.cpp -- see dd_typedart.h.

    C89 style, file-scope statics. Winsock idiom mirrors dd_update.cpp
    (non-blocking connect with a select() timeout, then a blocking send loop).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <winsockx.h>
#include <stdlib.h>
#include "dd_typedart.h"
#include "dd_udp.h"
#include "dd_launcher.h"
#include "dd_dc.h"
#include "dd_stbi.h"

#define TYPED_IMAGE_PORT   50580   /* raw image socket on the device          */
#define TYPED_CMD_PORT     8080    /* HTTP /cmd endpoint (resume)             */
#define TYPED_CONNECT_SEC  4       /* connect timeout                         */
#define TD_ID_MIN          1       /* art-addressable Type-D ids: 1-4 regular */
#define TD_ID_MAX          5       /* Type-D, 5 = XL. (6 = Expansion: no art.) */
#define TYPED_IO_MS        20000   /* send/recv timeout once connected (a full
                                      480x480 frame is ~460 KB; allow ample time
                                      for the device to drain + ack it)         */

                                      /* NPA1 header: magic, w, h, flags, reserved (u16 LE). 12 bytes. */
static void BuildHeader(unsigned char* h12, int w, int h) {
    h12[0] = 'N'; h12[1] = 'P'; h12[2] = 'A'; h12[3] = '1';
    h12[4] = (unsigned char)(w & 0xFF);  h12[5] = (unsigned char)((w >> 8) & 0xFF);
    h12[6] = (unsigned char)(h & 0xFF);  h12[7] = (unsigned char)((h >> 8) & 0xFF);
    h12[8] = 0; h12[9] = 0; h12[10] = 0; h12[11] = 0;
}

/* Native square panel size for a Type-D id: the XL (id 5) is 480x480, the
   regular Type-D units (1-4) are 240x240. */
static int NativeDim(int id) { return (id == 5) ? 480 : 240; }

/* Is cover art enabled for this id's class? XL (5) and the regular Type-D units
   (1-4) have independent toggles. (id 6 = Expansion: never.) */
static int ArtEnabledForId(int id) {
    if (id == 5)               return Dc_TypeDArtEnabled();
    if (id >= 1 && id <= 4)    return Dc_TypeDCtrlArtEnabled();
    return 0;
}

/* Open a blocking TCP connection to 'ip' (network order) on 'port'. Returns a
   connected socket or INVALID_SOCKET. */
static SOCKET ConnectIp(unsigned long ip, int port) {
    SOCKET sock;
    struct sockaddr_in sa;
    u_long nb = 1;
    int cr, tmo = TYPED_IO_MS;

    if (ip == 0) return INVALID_SOCKET;               /* not discovered */

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;
    ioctlsocket(sock, FIONBIO, &nb);

    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = (unsigned long)ip;

    cr = connect(sock, (struct sockaddr*)&sa, sizeof(sa));
    if (cr == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(sock); return INVALID_SOCKET; }
        {
            fd_set wset; TIMEVAL tv;
            FD_ZERO(&wset); FD_SET(sock, &wset);
            tv.tv_sec = TYPED_CONNECT_SEC; tv.tv_usec = 0;
            if (select(0, NULL, &wset, NULL, &tv) <= 0) { closesocket(sock); return INVALID_SOCKET; }
        }
    }
    nb = 0; ioctlsocket(sock, FIONBIO, &nb);          /* back to blocking */
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    return sock;
}

/* send all 'len' bytes (blocking socket); 1 on success. */
static int SendAll(SOCKET s, const unsigned char* p, int len) {
    int sent = 0, n;
    while (sent < len) {
        n = send(s, (const char*)(p + sent), len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

/* send one NPA1 frame to 'ip', waiting for the device's "OK" ack. 1 on success. */
static int SendFrameToIp(unsigned long ip, const unsigned short* px, int w, int h) {
    SOCKET sock;
    unsigned char hdr[12];
    int ok;

    if (!px || w <= 0 || h <= 0 || ip == 0) return 0;

    sock = ConnectIp(ip, TYPED_IMAGE_PORT);
    if (sock == INVALID_SOCKET) return 0;

    BuildHeader(hdr, w, h);
    ok = SendAll(sock, hdr, 12);
    if (ok) ok = SendAll(sock, (const unsigned char*)px, w * h * 2);

    if (ok) {
        /* Wait for the device's "OK" -- it only sends this once the WHOLE frame
           has been received. The caller (a game launch) tears down the network
           stack immediately after this returns, so without the ack the tail of
           the image in flight would be lost. SO_RCVTIMEO caps the wait. */
        char ack[2];
        int  got = 0, n;
        while (got < 2) {
            n = recv(sock, ack + got, 2 - got, 0);
            if (n <= 0) break;
            got += n;
        }
        if (got < 2 || ack[0] != 'O' || ack[1] != 'K') ok = 0;
    }

    closesocket(sock);   /* FIN after the ack handshake */
    return ok;
}

/* Diagnostic helper: send a frame to the XL (id 5). */
int TypeDArt_SendFrame(const unsigned short* px, int w, int h) {
    return SendFrameToIp(Udp_TypeDIp(5), px, w, h);
}

/* RGB565 (host order) from 8-bit components. */
static unsigned short Rgb565(int r, int g, int b) {
    return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

int TypeDArt_SendTestPattern(void) {
    static const unsigned char bars[8][3] = {
        {255,255,255},{255,255,0},{0,255,255},{0,255,0},
        {255,0,255},{255,0,0},{0,0,255},{0,0,0}
    };
    unsigned short* px;
    int x, y, bw, ok;

    px = (unsigned short*)malloc((size_t)TYPEDART_W * TYPEDART_H * 2);
    if (!px) return 0;
    bw = TYPEDART_W / 8;
    for (y = 0; y < TYPEDART_H; y++) {
        for (x = 0; x < TYPEDART_W; x++) {
            int b = x / bw; if (b > 7) b = 7;
            px[y * TYPEDART_W + x] = Rgb565(bars[b][0], bars[b][1], bars[b][2]);
        }
    }
    ok = TypeDArt_SendFrame(px, TYPEDART_W, TYPEDART_H);
    free(px);
    return ok;
}

/* Bilinear-fit an RGBA source into a square 'dim' x 'dim' RGB565 frame,
   preserving aspect with black letterbox bars. 'out' must hold dim*dim u16. */
static void ResizeLetterbox(const unsigned char* src, int sw, int sh,
    unsigned short* out, int dim) {
    int   i, x, y, dw, dh, ox, oy;
    float scale, s2;

    for (i = 0; i < dim * dim; i++) out[i] = 0;   /* black bars */
    if (!src || sw <= 0 || sh <= 0) return;

    scale = (float)dim / (float)sw;
    s2 = (float)dim / (float)sh;
    if (s2 < scale) scale = s2;

    dw = (int)((float)sw * scale + 0.5f);
    dh = (int)((float)sh * scale + 0.5f);
    if (dw > dim) dw = dim;
    if (dh > dim) dh = dim;
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    ox = (dim - dw) / 2;
    oy = (dim - dh) / 2;

    for (y = 0; y < dh; y++) {
        float sy = ((float)y + 0.5f) / (float)dh * (float)sh - 0.5f;
        int   iy = (int)sy;
        int   iy1;
        float fy;

        if (iy < 0) iy = 0;
        if (iy > sh - 1) iy = sh - 1;
        fy = sy - (float)iy;
        if (fy < 0.0f) fy = 0.0f;
        iy1 = iy + 1;
        if (iy1 > sh - 1) iy1 = sh - 1;

        for (x = 0; x < dw; x++) {
            float sx = ((float)x + 0.5f) / (float)dw * (float)sw - 0.5f;
            int   ix = (int)sx;
            int   ix1;
            float fx, w00, w10, w01, w11;
            const unsigned char* p00, * p10, * p01, * p11;
            int   r, g, b;

            if (ix < 0) ix = 0;
            if (ix > sw - 1) ix = sw - 1;
            fx = sx - (float)ix;
            if (fx < 0.0f) fx = 0.0f;
            ix1 = ix + 1;
            if (ix1 > sw - 1) ix1 = sw - 1;

            p00 = src + (iy * sw + ix) * 4;
            p10 = src + (iy * sw + ix1) * 4;
            p01 = src + (iy1 * sw + ix) * 4;
            p11 = src + (iy1 * sw + ix1) * 4;
            w00 = (1.0f - fx) * (1.0f - fy);
            w10 = fx * (1.0f - fy);
            w01 = (1.0f - fx) * fy;
            w11 = fx * fy;

            r = (int)(p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11 + 0.5f);
            g = (int)(p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11 + 0.5f);
            b = (int)(p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11 + 0.5f);
            if (r > 255) r = 255; if (r < 0) r = 0;
            if (g > 255) g = 255; if (g < 0) g = 0;
            if (b > 255) b = 255; if (b < 0) b = 0;

            out[(oy + y) * dim + (ox + x)] = Rgb565(r, g, b);
        }
    }
}

int TypeDArt_SendArtFor(const char* xbePath) {
    unsigned char* rgba;
    unsigned short* f480 = 0;
    unsigned short* f240 = 0;
    int             rw = 0, rh = 0, id, need480 = 0, need240 = 0, sentAny = 0;

    if (!xbePath || !xbePath[0]) return 0;

    /* which native sizes do the present + art-enabled units need? */
    for (id = TD_ID_MIN; id <= TD_ID_MAX; id++) {
        if (!Udp_TypeDIp(id) || !ArtEnabledForId(id)) continue;
        if (NativeDim(id) == 480) need480 = 1; else need240 = 1;
    }
    if (!need480 && !need240) return 0;             /* nothing to send to */

    rgba = Launcher_LoadArtRGBA(xbePath, &rw, &rh); /* resource-pack art */
    if (!rgba || rw <= 0 || rh <= 0) {
        if (rgba) DD_StbFree(rgba);
        return 0;                                   /* no pack art for this title */
    }

    /* resize once per distinct size, then reuse for every unit of that size */
    if (need480) {
        f480 = (unsigned short*)malloc((size_t)480 * 480 * 2);
        if (f480) ResizeLetterbox(rgba, rw, rh, f480, 480);
    }
    if (need240) {
        f240 = (unsigned short*)malloc((size_t)240 * 240 * 2);
        if (f240) ResizeLetterbox(rgba, rw, rh, f240, 240);
    }
    DD_StbFree(rgba);

    /* push to each present unit at its native size (sequential; the launch
       loading screen masks the cumulative transfer time). Largest panels go
       FIRST: the XL's 480x480 frame is ~4x the payload of a 240x240 Type-D, so
       firing it before the small units gives the long transfer the earliest,
       freshest window -- least likely to be squeezed by the launch hand-off
       tearing down networking. Two passes (480 then 240) order by payload size
       rather than by id, so it stays correct regardless of id assignment. */
    {
        int pass;
        for (pass = 0; pass < 2; pass++) {
            int wantDim = (pass == 0) ? 480 : 240;
            for (id = TD_ID_MIN; id <= TD_ID_MAX; id++) {
                unsigned long   ip = Udp_TypeDIp(id);
                int             dim;
                unsigned short* fr;
                if (!ip || !ArtEnabledForId(id)) continue;
                dim = NativeDim(id);
                if (dim != wantDim) continue;
                fr = (dim == 480) ? f480 : f240;
                if (!fr) continue;
                if (SendFrameToIp(ip, fr, dim, dim)) sentAny = 1;
            }
        }
    }

    if (f480) free(f480);
    if (f240) free(f240);
    return sentAny;
}

static int ResumeIp(unsigned long ip) {
    SOCKET sock;
    int ok;
    static const char* req =
        "GET /cmd?c=0007 HTTP/1.0\r\nHost: typed\r\nConnection: close\r\n\r\n";
    int len = 0; while (req[len]) len++;

    sock = ConnectIp(ip, TYPED_CMD_PORT);
    if (sock == INVALID_SOCKET) return 0;
    ok = SendAll(sock, (const unsigned char*)req, len);
    closesocket(sock);
    return ok;
}

int TypeDArt_Resume(void) {
    int id, okAny = 0;
    for (id = TD_ID_MIN; id <= TD_ID_MAX; id++) {
        unsigned long ip = Udp_TypeDIp(id);
        if (!ip) continue;
        if (ResumeIp(ip)) okAny = 1;
    }
    return okAny;
}

int TypeDArt_Present(void) {
    int id;
    for (id = TD_ID_MIN; id <= TD_ID_MAX; id++)
        if (Udp_TypeDIp(id)) return 1;
    return 0;
}

int TypeDArt_WillSend(void) {
    int id;
    for (id = TD_ID_MIN; id <= TD_ID_MAX; id++)
        if (Udp_TypeDIp(id) && ArtEnabledForId(id)) return 1;
    return 0;
}

void TypeDArt_BootResumeTick(void) {
    /* Once per dash boot, the first frame the device is discovered, tell it to
       drop any art held from the previous session and return to its slideshow.
       Deferred until discovery latches the IP (the beacon arrives ~3s in), and
       guarded so it fires a single time -- a later manual push is left alone. */
    static int s_done = 0;
    if (s_done) return;
    if (!TypeDArt_Present()) return;
    TypeDArt_Resume();      /* best-effort one shot */
    s_done = 1;
}