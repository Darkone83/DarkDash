/*---------------------------------------------------------------------------
    dd_udp.cpp -- see dd_udp.h.

    A single reusable broadcast socket (non-blocking DGRAM with SO_BROADCAST),
    created lazily and kept open for reuse. Mirrors the Winsock idioms DarkDash
    already uses in dd_ftp / dd_ntp. C89 style, file-scope statics.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <winsockx.h>
#include "dd_udp.h"

static SOCKET s_sock = INVALID_SOCKET;
static int    s_inited = 0;

/* ---- discovery registry (declared up here: used by Udp_Shutdown below) --- */
typedef struct {
    int     port;            /* device's UDP port                              */
    const char* sig;         /* signature substring in its reply/advert        */
    int     active;          /* 1 = we must poll it; 0 = it self-advertises     */
    SOCKET  sock;            /* bound listen socket for this device's port      */
    DWORD   lastSeen;        /* GetTickCount of last signature match (0 = never)*/
    DWORD   addr;            /* last-known device IP (network order, 0 = none)  */
} DiscoDev;

static DiscoDev s_dev[UDP_DEV_COUNT] = {
    /* RGB:  port 7777, advertises "XBOX RGB", passive */
    { 7777,  "XBOX RGB", 0, INVALID_SOCKET, 0, 0 },
    /* OXFP: port 32123, replies "OXFP", active poll    */
    { 32123, "OXFP",     1, INVALID_SOCKET, 0, 0 }
};

static DWORD s_oxfpPollTimer = 0;
static char  s_rx[1600];
#define DISCO_STALE_MS   25000UL   /* present if seen within this window       */
#define OXFP_POLL_MS      4000UL   /* how often to ping the (silent) OXFP      */
#define DISCO_RXBUF       1600

/* Create (once) a non-blocking, broadcast-enabled UDP socket. Returns 1 if a
   usable socket exists afterward. */
static int EnsureSocket(void) {
    BOOL bcast = TRUE;
    unsigned long nb = 1;

    if (s_sock != INVALID_SOCKET) return 1;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock == INVALID_SOCKET) return 0;

    /* allow sending to the limited broadcast address */
    if (setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST,
        (const char*)&bcast, sizeof(bcast)) != 0) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        return 0;
    }

    /* non-blocking so a send can never stall the dashboard */
    ioctlsocket(s_sock, FIONBIO, &nb);
    return 1;
}

void Udp_Init(void) {
    s_inited = 1;
    /* socket is created lazily on first broadcast (network may not be up yet) */
}

void Udp_Shutdown(void) {
    int i;
    if (s_sock != INVALID_SOCKET) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
    for (i = 0; i < UDP_DEV_COUNT; i++) {
        if (s_dev[i].sock != INVALID_SOCKET) {
            closesocket(s_dev[i].sock);
            s_dev[i].sock = INVALID_SOCKET;
        }
    }
    s_inited = 0;
}

int Udp_Broadcast(int port, const void* data, int len) {
    SOCKADDR_IN sa;
    int sent;

    if (!s_inited) Udp_Init();
    if (!data || len <= 0) return 0;
    if (!EnsureSocket()) return 0;

    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = INADDR_BROADCAST;   /* 255.255.255.255 */

    sent = sendto(s_sock, (const char*)data, len, 0,
        (struct sockaddr*)&sa, sizeof(sa));
    return (sent == len) ? 1 : 0;
}

/* ============================================================================
   Device discovery
   ============================================================================ */

   /* substring search (no CRT strstr dependency) */
static int ContainsSig(const char* hay, int hayLen, const char* needle) {
    int nl = 0, i, j;
    while (needle[nl]) nl++;
    if (nl == 0 || hayLen < nl) return 0;
    for (i = 0; i <= hayLen - nl; i++) {
        for (j = 0; j < nl; j++) if (hay[i + j] != needle[j]) break;
        if (j == nl) return 1;
    }
    return 0;
}

/* bind a non-blocking UDP socket to INADDR_ANY:port for receiving */
static SOCKET BindListen(int port) {
    SOCKET s;
    SOCKADDR_IN sa;
    BOOL reuse = TRUE, bcast = TRUE;
    unsigned long nb = 1;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&bcast, sizeof(bcast));

    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((u_short)port);
    if (bind(s, (SOCKADDR*)&sa, sizeof(sa)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

/* Send a datagram to 255.255.255.255:port FROM an already-bound socket. Used to
   probe the silent OXFP from its own listen socket: OXFP replies unicast to the
   requester's source port, so the answer must arrive on the socket we drain --
   sending via the shared broadcast socket (s_sock, an ephemeral source port)
   sent the reply to a socket nothing reads, so OXFP was never detected. */
static int SendBroadcastFrom(SOCKET s, int port, const void* data, int len) {
    SOCKADDR_IN sa;
    int sent;
    if (s == INVALID_SOCKET || !data || len <= 0) return 0;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = INADDR_BROADCAST;
    sent = sendto(s, (const char*)data, len, 0, (struct sockaddr*)&sa, sizeof(sa));
    return (sent == len) ? 1 : 0;
}

/* drain a device's socket; stamp lastSeen + addr on a signature match */
static void DrainDevice(DiscoDev* dv) {
    SOCKADDR_IN from;
    int fromLen, n;

    if (dv->sock == INVALID_SOCKET) return;
    for (;;) {
        fromLen = sizeof(from);
        n = recvfrom(dv->sock, s_rx, DISCO_RXBUF - 1, 0,
            (SOCKADDR*)&from, &fromLen);
        if (n <= 0) break;                /* WSAEWOULDBLOCK or closed -> done   */
        s_rx[n] = 0;
        if (ContainsSig(s_rx, n, dv->sig)) {
            dv->lastSeen = GetTickCount();
            dv->addr = from.sin_addr.s_addr;
        }
    }
}

void Udp_DiscoTick(void) {
    DWORD now;
    int i;

    if (!s_inited) Udp_Init();
    now = GetTickCount();

    /* lazily bind each device's listen socket (network may be down at boot) */
    for (i = 0; i < UDP_DEV_COUNT; i++) {
        if (s_dev[i].sock == INVALID_SOCKET)
            s_dev[i].sock = BindListen(s_dev[i].port);
        DrainDevice(&s_dev[i]);
    }

    /* OXFP is silent: poll it so it answers. The probe MUST go out from the
       OXFP listen socket (bound to 32123), because the firmware replies unicast
       to the probe's source port -- only then does DrainDevice above see it. */
    if (s_oxfpPollTimer == 0 || (now - s_oxfpPollTimer) >= OXFP_POLL_MS) {
        static const char k_ping[] = "{\"op\":\"ping\"}";
        s_oxfpPollTimer = now;
        SendBroadcastFrom(s_dev[UDP_DEV_OXFP].sock, s_dev[UDP_DEV_OXFP].port,
            k_ping, (int)(sizeof(k_ping) - 1));
    }
}

int Udp_Present(int dev) {
    DWORD now;
    if (dev < 0 || dev >= UDP_DEV_COUNT) return 0;
    if (s_dev[dev].lastSeen == 0) return 0;
    now = GetTickCount();
    return ((now - s_dev[dev].lastSeen) <= DISCO_STALE_MS) ? 1 : 0;
}

int Udp_SendToDevice(int dev, const void* data, int len) {
    SOCKADDR_IN sa;
    int sent;

    if (dev < 0 || dev >= UDP_DEV_COUNT) return 0;
    if (!data || len <= 0) return 0;
    if (!EnsureSocket()) return 0;

    /* prefer the device's known unicast address; fall back to broadcast */
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)s_dev[dev].port);
    sa.sin_addr.s_addr = s_dev[dev].addr ? s_dev[dev].addr : INADDR_BROADCAST;

    sent = sendto(s_sock, (const char*)data, len, 0,
        (struct sockaddr*)&sa, sizeof(sa));
    return (sent == len) ? 1 : 0;
}