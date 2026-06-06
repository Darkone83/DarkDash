/*---------------------------------------------------------------------------
    dd_update.cpp -- inline OTA self-updater core (see dd_update.h).

    Ported from XbDiag Update.cpp. The non-blocking version-check state
    machine is intact; the download + XBA extract run as one bounded step
    driven from Upd_Tick (no full-screen render loop -- the Settings panel
    draws from the status getters). DarkDash paths/host substituted for
    XbDiag's.

    Build: MSVC2003/C89 style; file-scope statics; inlined StrCopy/StrLen/
    IntToStr/AppendStr (no CRT sprintf/strlen).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <winsockx.h>
#include "xboxinternals.h"
#include "dd_update.h"
#include "dd_net.h"     /* shared network-stack owner -- do NOT XNetStartup here */
#include "xba.h"

/* ---- config ----------------------------------------------------------- */

static const char* k_host = "darkone83.myddns.me";
static const int   k_httpPort = 8008;
static const char* k_verPath = "/darkdash/DarkDash.ver";
static const char* k_xbaServerPath = "/darkdash/update.xba";
static const char* k_logPath = "/darkdash/log.chg";
static const char* k_xbaTempPath = "D:\\update.xba";
static const char* k_xbeDest = "D:\\default.xbe";   /* relaunch target */
static const char* k_extractDir = "D:\\";              /* extract over install */

/* ---- string helpers (no CRT) ------------------------------------------ */

static int StrLen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void StrCopy(char* dst, int cap, const char* src) {
    int i = 0; if (cap <= 0) return;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void AppendStr(char* dst, int cap, const char* src) {
    int dlen = StrLen(dst), slen = StrLen(src), space = cap - dlen - 1, i;
    for (i = 0; i < slen && i < space; i++) dst[dlen + i] = src[i];
    dst[dlen + (slen < space ? slen : space)] = 0;
}

static void IntToStr(int v, char* buf, int cap) {
    char tmp[16]; int n = 0, neg = 0, i; if (cap <= 0) return;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 15) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    i = 0;
    if (neg && i < cap - 1) buf[i++] = '-';
    while (n > 0 && i < cap - 1) buf[i++] = tmp[--n];
    buf[i] = 0;
}

static int IsWrapSpace(char c) { return c == ' ' || c == '\t'; }

/* ---- version parse / compare ------------------------------------------ */

static void ParseVerParts(const char* s, int v[3]) {
    int field = 0, i;
    v[0] = v[1] = v[2] = 0;
    for (i = 0; s[i] && field < 3; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') v[field] = v[field] * 10 + (c - '0');
        else if (c == '.')        field++;
        else break;
    }
}
static int VerCmp(const char* a, const char* b) {
    int pa[3], pb[3], i;
    ParseVerParts(a, pa); ParseVerParts(b, pb);
    for (i = 0; i < 3; i++) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return  1;
    }
    return 0;
}
static void StripWhitespace(char* buf) {
    int len = StrLen(buf), start = 0, i;
    while (len > 0) {
        char c = buf[len - 1];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') buf[--len] = 0;
        else break;
    }
    while (buf[start] == ' ' || buf[start] == '\t') start++;
    if (start > 0) { i = 0; while (buf[start + i]) { buf[i] = buf[start + i]; i++; } buf[i] = 0; }
}

/* ---- HTTP response helpers -------------------------------------------- */

static int FindHeaderEnd(const char* buf, int len) {
    int i;
    for (i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    return -1;
}
static int GetHttpStatus(const char* buf, int len) {
    int i = 0;
    while (i < len && buf[i] != ' ') i++;
    while (i < len && buf[i] == ' ') i++;
    if (i + 2 < len) return (buf[i] - '0') * 100 + (buf[i + 1] - '0') * 10 + (buf[i + 2] - '0');
    return 0;
}
static DWORD ParseContentLength(const char* buf, int bodyStart) {
    /* scan headers for "Content-Length:" */
    const char* k = "content-length:";
    int i, j;
    for (i = 0; i < bodyStart; i++) {
        int m = 1;
        for (j = 0; k[j]; j++) {
            char c = buf[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != k[j]) { m = 0; break; }
        }
        if (m) {
            DWORD v = 0; int p = i + (int)StrLen(k);
            while (buf[p] == ' ') p++;
            while (buf[p] >= '0' && buf[p] <= '9') { v = v * 10 + (buf[p] - '0'); p++; }
            return v;
        }
    }
    return 0;
}
static const char* FindBody(const char* buf, int len, DWORD* outCL) {
    int hs = FindHeaderEnd(buf, len);
    if (hs < 0) return NULL;
    if (outCL) *outCL = ParseContentLength(buf, hs);
    return buf + hs;
}

/* ---- state ------------------------------------------------------------ */

enum {
    ST_IDLE = 0, ST_NET_INIT, ST_DNS, ST_CONNECT_VER, ST_SEND_VER,
    ST_RECV_VER, ST_COMPARE, ST_UPTODATE, ST_AVAIL,
    ST_DO_UPDATE, ST_DONE, ST_ERROR
};

static int        s_st = ST_IDLE;
static int        s_netUp = 0;
static XNDNS* s_dns = NULL;
static IN_ADDR    s_serverAddr;
static SOCKET     s_sock = INVALID_SOCKET;
static char       s_recvBuf[4096];
static int        s_recvLen = 0;
static char       s_localVer[32] = { 0 };
static char       s_dispVer[32] = { 0 };   /* display version (never empty) */
static char       s_remoteVer[32] = { 0 };
static char       s_errorMsg[80] = { 0 };
static char       s_xbaDetail[128] = { 0 };
static DWORD      s_netInitStart = 0;
static DWORD      s_dlTotal = 0, s_dlRecv = 0;
static int        s_extractDone = 0, s_extractTotal = 0;
static int        s_inExtract = 0;     /* progress phase flag */
static UpdRenderFn s_renderFn = 0;     /* render pump during blocking download */

/* changelog */
static char       s_changelog[4096] = { 0 };
static int        s_changelogLen = 0;
static int        s_changelogReady = 0;

/* ---- socket / net plumbing -------------------------------------------- */

static void NetEnsure(void) {
    /* Use the SHARED stack owned by dd_net -- never run a second XNetStartup
       here. XNetStartup is reference-counted: a duplicate startup from the
       updater meant Net_Restart()'s single XNetCleanup could only drop the count
       to 1, so the stack never actually tore down and an in-place network
       re-config silently failed (FTP/UDP couldn't rebind until a full relaunch).
       Net_Start() is idempotent, so this just guarantees the stack is up. */
    if (s_netUp) return;
    Net_Start();
    s_netUp = 1;
}
static void CloseSock(void) {
    if (s_sock != INVALID_SOCKET) { closesocket(s_sock); s_sock = INVALID_SOCKET; }
}
static void SetError(const char* msg) {
    StrCopy(s_errorMsg, sizeof(s_errorMsg), msg);
    s_st = ST_ERROR;
    CloseSock();
    if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
}

static int BeginConnect(int nextState) {
    struct sockaddr_in sa;
    u_long nb = 1;
    int r;
    CloseSock();
    s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_sock == INVALID_SOCKET) { SetError("socket failed"); return 0; }
    ioctlsocket(s_sock, FIONBIO, &nb);
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)k_httpPort);
    sa.sin_addr = s_serverAddr;
    r = connect(s_sock, (struct sockaddr*)&sa, sizeof(sa));
    if (r == 0 || WSAGetLastError() == WSAEWOULDBLOCK) { s_st = nextState; return 1; }
    SetError("connect failed"); return 0;
}
static int PollConnect(void) {
    fd_set wfds, efds; TIMEVAL tv;
    FD_ZERO(&wfds); FD_SET(s_sock, &wfds);
    FD_ZERO(&efds); FD_SET(s_sock, &efds);
    tv.tv_sec = 0; tv.tv_usec = 0;
    if (select(0, NULL, &wfds, &efds, &tv) == SOCKET_ERROR) { SetError("select failed"); return 0; }
    if (FD_ISSET(s_sock, &efds)) { SetError("connect refused"); return 0; }
    return FD_ISSET(s_sock, &wfds) != 0;
}
static int SendGet(const char* path) {
    char req[256];
    int total, sent = 0, n;
    StrCopy(req, sizeof(req), "GET ");
    AppendStr(req, sizeof(req), path);
    AppendStr(req, sizeof(req), " HTTP/1.0\r\nHost: ");
    AppendStr(req, sizeof(req), k_host);
    AppendStr(req, sizeof(req), "\r\nConnection: close\r\n\r\n");
    total = StrLen(req);
    while (sent < total) {
        n = send(s_sock, req + sent, total - sent, 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
            SetError("send failed"); return 0;
        }
        sent += n;
    }
    return 1;
}

/* ---- local version --------------------------------------------------- */

void Upd_Init(const char* localVersion) {
    /* Local version comes from D:\DarkDash.ver on disk. If the file is absent
       we leave the local version EMPTY -- the compare then flags an update as
       available, and the download writes DarkDash.ver as its final step. The
       compiled-in DARKDASH_VERSION is only a display fallback. */
    HANDLE h;
    s_localVer[0] = 0;
    h = CreateFileA("D:\\DarkDash.ver", GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD got = 0;
        char buf[32];
        if (ReadFile(h, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
            buf[got] = 0;
            StrCopy(s_localVer, sizeof(s_localVer), buf);
            StripWhitespace(s_localVer);
        }
        CloseHandle(h);
    }
    /* keep the compiled-in string for display when there's no file yet */
    if (s_localVer[0] == 0 && localVersion)
        StrCopy(s_dispVer, sizeof(s_dispVer), localVersion);
    else
        StrCopy(s_dispVer, sizeof(s_dispVer), s_localVer);
    s_st = ST_IDLE;
}

/* ---- blocking download (one file). Matches XbDiag DoDownload. --------- */

static int DoDownload(const char* path, const char* dest, int showProgress) {
    SOCKET sock;
    struct sockaddr_in sa;
    u_long nb = 1;
    int cr, tmo = 10000, bodyStart, overflow;
    char req[256], hdrBuf[2048], dlBuf[4096];
    int hdrLen = 0, n;
    DWORD cl, totalRecv = 0, wr;
    HANDLE hf;

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return 0;
    ioctlsocket(sock, FIONBIO, &nb);
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)k_httpPort);
    sa.sin_addr = s_serverAddr;
    cr = connect(sock, (struct sockaddr*)&sa, sizeof(sa));
    if (cr == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(sock); return 0; }
        {
            fd_set wset; TIMEVAL tv;
            FD_ZERO(&wset); FD_SET(sock, &wset);
            tv.tv_sec = 5; tv.tv_usec = 0;
            if (select(0, NULL, &wset, NULL, &tv) <= 0) { closesocket(sock); return 0; }
        }
    }
    nb = 0; ioctlsocket(sock, FIONBIO, &nb);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));

    StrCopy(req, sizeof(req), "GET ");
    AppendStr(req, sizeof(req), path);
    AppendStr(req, sizeof(req), " HTTP/1.0\r\nHost: ");
    AppendStr(req, sizeof(req), k_host);
    AppendStr(req, sizeof(req), "\r\nConnection: close\r\n\r\n");
    if (send(sock, req, StrLen(req), 0) <= 0) { closesocket(sock); return 0; }

    while (hdrLen < (int)sizeof(hdrBuf) - 1) {
        n = recv(sock, hdrBuf + hdrLen, (int)sizeof(hdrBuf) - 1 - hdrLen, 0);
        if (n <= 0) { closesocket(sock); return 0; }
        hdrLen += n; hdrBuf[hdrLen] = 0;
        if (FindHeaderEnd(hdrBuf, hdrLen) >= 0) break;
    }
    bodyStart = FindHeaderEnd(hdrBuf, hdrLen);
    if (bodyStart < 0) { closesocket(sock); return 0; }
    if (GetHttpStatus(hdrBuf, hdrLen) != 200) { closesocket(sock); return 0; }

    cl = ParseContentLength(hdrBuf, bodyStart);
    if (showProgress) { s_dlTotal = cl; s_dlRecv = 0; }

    hf = CreateFileA(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { closesocket(sock); return 0; }

    overflow = hdrLen - bodyStart;
    if (overflow > 0) {
        WriteFile(hf, hdrBuf + bodyStart, (DWORD)overflow, &wr, NULL);
        totalRecv += wr;
        if (showProgress) s_dlRecv = totalRecv;
    }
    {
        DWORD pumpAccum = 0;
        for (;;) {
            n = recv(sock, dlBuf, sizeof(dlBuf), 0);
            if (n <= 0) break;
            WriteFile(hf, dlBuf, (DWORD)n, &wr, NULL);
            totalRecv += wr;
            if (showProgress) {
                s_dlRecv = totalRecv;
                /* pump a render every ~64KB so the bar advances during the
                   blocking download (XbDiag pattern). */
                pumpAccum += wr;
                if (pumpAccum >= 65536) {
                    if (s_renderFn) s_renderFn();
                    pumpAccum = 0;
                }
            }
            if (cl > 0 && totalRecv >= cl) break;
        }
    }
    FlushFileBuffers(hf);
    CloseHandle(hf);
    closesocket(sock);
    if (totalRecv == 0) { DeleteFileA(dest); return 0; }
    return 1;
}

/* XBA extract progress -> our progress fields */
static void XbaProgressCb(int filesDone, int filesTotal, DWORD bytesDone, DWORD bytesTotal) {
    (void)bytesDone; (void)bytesTotal;
    s_extractDone = filesDone; s_extractTotal = filesTotal;
}

/* ---- public driver API ------------------------------------------------ */

void Upd_StartCheck(void) {
    CloseSock();
    if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
    s_recvLen = 0;
    s_recvBuf[0] = 0;
    s_remoteVer[0] = 0;
    s_errorMsg[0] = 0;
    s_dlTotal = s_dlRecv = 0;
    NetEnsure();
    s_netInitStart = GetTickCount();
    s_st = ST_NET_INIT;
}

void Upd_StartDownload(void) {
    if (s_st == ST_AVAIL || s_st == ST_UPTODATE) s_st = ST_DO_UPDATE;
}

void Upd_Cancel(void) {
    CloseSock();
    if (s_dns) { XNetDnsRelease(s_dns); s_dns = NULL; }
    s_st = ST_IDLE;
}

void Upd_Relaunch(void) {
    LAUNCH_DATA ld; ZeroMemory(&ld, sizeof(ld));
    XLaunchNewImage(k_xbeDest, &ld);
    for (;;) {}     /* never returns on success */
}

void Upd_SetRenderFn(UpdRenderFn fn) {
    s_renderFn = fn;
}

/* fetch changelog into s_changelog (blocking, best-effort) */
static void FetchChangelog(void) {
    if (s_changelogReady) return;
    if (DoDownload(k_logPath, "D:\\log.chg", 0)) {
        HANDLE h = CreateFileA("D:\\log.chg", GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD got = 0;
            ReadFile(h, s_changelog, sizeof(s_changelog) - 1, &got, NULL);
            s_changelog[got] = 0;
            s_changelogLen = (int)got;
            CloseHandle(h);
        }
        DeleteFileA("D:\\log.chg");
    }
    s_changelogReady = 1;
}

void Upd_Tick(void) {
    switch (s_st) {
    case ST_NET_INIT: {
        XNADDR xna; DWORD st;
        ZeroMemory(&xna, sizeof(xna));
        st = XNetGetTitleXnAddr(&xna);
        if (st == XNET_GET_XNADDR_PENDING) {
            if (GetTickCount() - s_netInitStart > 5000) SetError("No network link");
            break;
        }
        if ((st & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0) { SetError("No network link"); break; }
        if (XNetDnsLookup(k_host, NULL, &s_dns) != 0 || !s_dns) { SetError("DNS lookup failed"); break; }
        s_st = ST_DNS;
        break;
    }
    case ST_DNS: {
        if (!s_dns) { SetError("DNS handle null"); break; }
        if (s_dns->iStatus == WSAEINPROGRESS) break;
        if (s_dns->iStatus != 0) { XNetDnsRelease(s_dns); s_dns = NULL; SetError("DNS failed"); break; }
        s_serverAddr = s_dns->aina[0];
        XNetDnsRelease(s_dns); s_dns = NULL;
        BeginConnect(ST_CONNECT_VER);
        break;
    }
    case ST_CONNECT_VER: {
        if (!PollConnect()) break;
        s_recvLen = 0; s_recvBuf[0] = 0;
        if (!SendGet(k_verPath)) break;
        s_st = ST_RECV_VER;
        break;
    }
    case ST_RECV_VER: {
        int space = (int)sizeof(s_recvBuf) - s_recvLen - 1, n;
        if (space <= 0) { s_st = ST_COMPARE; break; }
        n = recv(s_sock, s_recvBuf + s_recvLen, space, 0);
        if (n > 0) { s_recvLen += n; s_recvBuf[s_recvLen] = 0; }
        else if (n == 0) { CloseSock(); s_st = ST_COMPARE; }
        else if (WSAGetLastError() != WSAEWOULDBLOCK) SetError("Connection lost");
        break;
    }
    case ST_COMPARE: {
        int status = GetHttpStatus(s_recvBuf, s_recvLen);
        const char* body; DWORD cl = 0;
        /* a missing/unavailable version file (404 etc.) just means "nothing to
           update to" -- report up-to-date rather than a scary error. */
        if (status != 200) { s_st = ST_UPTODATE; break; }
        body = FindBody(s_recvBuf, s_recvLen, &cl);
        if (!body) { s_st = ST_UPTODATE; break; }
        StrCopy(s_remoteVer, sizeof(s_remoteVer), body);
        StripWhitespace(s_remoteVer);
        if (s_remoteVer[0] == 0) { s_st = ST_UPTODATE; break; }
        s_st = (s_localVer[0] == 0 || VerCmp(s_remoteVer, s_localVer) > 0)
            ? ST_AVAIL : ST_UPTODATE;
        FetchChangelog();   /* best-effort, after the check */
        break;
    }
    case ST_DO_UPDATE: {
        XbaResult xr;
        /* 1. download the archive */
        s_st = ST_DO_UPDATE;        /* (panel reads phase via Upd_State below) */
        s_inExtract = 0;
        s_dlTotal = 0; s_dlRecv = 0;
        if (!DoDownload(k_xbaServerPath, k_xbaTempPath, 1)) {
            DeleteFileA(k_xbaTempPath);
            SetError("download failed"); break;
        }
        /* 2. extract over the install */
        s_inExtract = 1;
        s_extractDone = 0; s_extractTotal = 0;
        s_xbaDetail[0] = 0;
        xr = Xba_Extract(k_xbaTempPath, k_extractDir, XbaProgressCb,
            s_xbaDetail, sizeof(s_xbaDetail));
        DeleteFileA(k_xbaTempPath);
        if (xr != XBA_OK) {
            char msg[80];
            StrCopy(msg, sizeof(msg), "extract failed: ");
            AppendStr(msg, sizeof(msg), Xba_ResultStr(xr));
            SetError(msg); break;
        }
        /* 3. write the new .ver last (confirms success) */
        DoDownload(k_verPath, "D:\\DarkDash.ver", 0);
        s_st = ST_DONE;
        break;
    }
    default: break;
    }
}

/* ---- status getters --------------------------------------------------- */

int Upd_State(void) {
    switch (s_st) {
    case ST_IDLE:       return UPD_IDLE;
    case ST_NET_INIT:
    case ST_DNS:
    case ST_CONNECT_VER:
    case ST_SEND_VER:
    case ST_RECV_VER:
    case ST_COMPARE:    return UPD_CHECKING;
    case ST_UPTODATE:   return UPD_UPTODATE;
    case ST_AVAIL:      return UPD_AVAILABLE;
    case ST_DO_UPDATE:  return s_inExtract ? UPD_EXTRACTING : UPD_DOWNLOADING;
    case ST_DONE:       return UPD_DONE;
    case ST_ERROR:      return UPD_ERROR;
    }
    return UPD_IDLE;
}
const char* Upd_LocalVersion(void) { return s_dispVer[0] ? s_dispVer : "unknown"; }
const char* Upd_RemoteVersion(void) { return s_remoteVer; }
const char* Upd_Error(void) { return s_errorMsg; }

int Upd_Progress(void) {
    if (s_inExtract) {
        if (s_extractTotal > 0) return s_extractDone * 100 / s_extractTotal;
        return 0;
    }
    if (s_dlTotal > 0) {
        return (int)((s_dlRecv * 100) / s_dlTotal);
    }
    return 0;
}

const char* Upd_Changelog(void) { return s_changelog; }
int  Upd_ChangelogReady(void) { return s_changelogReady; }