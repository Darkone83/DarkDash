/*---------------------------------------------------------------------------
    dd_ntp.cpp -- see dd_ntp.h.

    Minimal SNTP client. Sends a 48-byte client packet, reads the server reply,
    pulls the transmit timestamp (seconds since 1900-01-01) from bytes 40..43,
    converts NTP epoch -> Win32 FILETIME (100ns since 1601), and writes UTC to
    the system clock (the Xbox kernel applies the EEPROM timezone on readback).

    C89 style. No CRT str*. Uses winsock UDP, same stack dd_ftp/dd_net bring up.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_ntp.h"
#include "dd_time.h"
#include "dd_net.h"
#include "dd_sysinfo.h"

/* a couple of well-known NTP servers; try in order until one answers */
static const char* const k_servers[] = {
    "pool.ntp.org",
    "time.nist.gov",
    "time.windows.com",
    0
};

#define NTP_PORT        123
#define NTP_PKT_LEN     48
#define NTP_DNS_MS      3000
#define NTP_RECV_MS     3000   /* per-server receive timeout */

/* seconds between 1900-01-01 (NTP epoch) and 1970-01-01 (Unix) */
#define NTP_UNIX_DELTA  2208988800UL

enum { NA_IDLE = 0, NA_DNS, NA_RECV };
static int       s_naState = NA_IDLE;
static int       s_naServer = 0;
static XNDNS* s_naDns = NULL;
static WSAEVENT  s_naEvent = 0;
static SOCKET    s_naSock = INVALID_SOCKET;
static DWORD     s_naDeadline = 0;

static int PacketUnixSecs(const BYTE* pkt, int got, unsigned long* unixSecs) {
    unsigned long ntpSecs;
    if (!pkt || !unixSecs || got < NTP_PKT_LEN) return 0;
    ntpSecs = ((unsigned long)pkt[40] << 24) | ((unsigned long)pkt[41] << 16) |
        ((unsigned long)pkt[42] << 8) | (unsigned long)pkt[43];
    if (ntpSecs <= NTP_UNIX_DELTA) return 0;
    *unixSecs = ntpSecs - NTP_UNIX_DELTA;
    return 1;
}

static int ApplyUnixSecs(unsigned long unixSecs) {
    ULONGLONG ft;
    FILETIME  filetime;
    SYSTEMTIME stime;
    SysClock  c;

    ft = ((ULONGLONG)unixSecs + 11644473600ULL) * 10000000ULL;
    filetime.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFULL);
    filetime.dwHighDateTime = (DWORD)(ft >> 32);
    if (!FileTimeToSystemTime(&filetime, &stime)) return 0;

    c.year = stime.wYear; c.mon = stime.wMonth; c.day = stime.wDay;
    c.hour = stime.wHour; c.min = stime.wMinute; c.sec = stime.wSecond;
    c.dow = stime.wDayOfWeek;
    return Sys_SetClockDirect(&c);
}

/* Resolve a host to an IPv4 address (network order). 0 on failure.
   Xbox has no gethostbyname -- use XNetDnsLookup (async; we poll its status
   with a bounded wait so we never hang). */
static unsigned long ResolveHost(const char* host) {
    unsigned long ip;
    XNDNS* pdns = NULL;
    WSAEVENT hev;
    INT     rc;
    DWORD   start;

    ip = inet_addr(host);
    if (ip != INADDR_NONE) return ip;       /* already dotted-quad */

    hev = WSACreateEvent();
    rc = XNetDnsLookup(host, hev, &pdns);
    if (rc != 0 || !pdns) {
        if (hev) WSACloseEvent(hev);
        return 0;
    }

    /* poll until resolved or ~3s elapse */
    start = GetTickCount();
    while (pdns->iStatus == WSAEINPROGRESS) {
        if (GetTickCount() - start > 3000) break;
        Sleep(20);
    }

    ip = 0;
    if (pdns->iStatus == 0 && pdns->cina > 0)
        ip = pdns->aina[0].s_addr;

    XNetDnsRelease(pdns);
    if (hev) WSACloseEvent(hev);
    return ip;
}

/* Query one server. Fills *unixSecs (UTC seconds since 1970). 1 on success. */
static int QueryServer(const char* host, unsigned long* unixSecs) {
    SOCKET s;
    struct sockaddr_in sa;
    BYTE pkt[NTP_PKT_LEN];
    unsigned long ip;
    int got;
    fd_set rfds;
    struct timeval tv;

    ip = ResolveHost(host);
    if (!ip) return 0;

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;

    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)NTP_PORT);
    sa.sin_addr.s_addr = ip;

    /* client request: LI=0, VN=3, Mode=3 (client) -> first byte 0x1B, rest 0 */
    ZeroMemory(pkt, sizeof(pkt));
    pkt[0] = 0x1B;

    if (sendto(s, (const char*)pkt, NTP_PKT_LEN, 0,
        (struct sockaddr*)&sa, sizeof(sa)) != NTP_PKT_LEN) {
        closesocket(s);
        return 0;
    }

    /* wait for a reply with a bounded timeout so we never hang */
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    tv.tv_sec = NTP_RECV_MS / 1000;
    tv.tv_usec = (NTP_RECV_MS % 1000) * 1000;
    if (select(0, &rfds, NULL, NULL, &tv) <= 0) {
        closesocket(s);
        return 0;
    }

    got = recv(s, (char*)pkt, NTP_PKT_LEN, 0);
    closesocket(s);
    if (got < NTP_PKT_LEN) return 0;

    return PacketUnixSecs(pkt, got, unixSecs);
}

static void AsyncReleaseDns(void) {
    if (s_naDns) { XNetDnsRelease(s_naDns); s_naDns = NULL; }
    if (s_naEvent) { WSACloseEvent(s_naEvent); s_naEvent = 0; }
}

static void AsyncReleaseSock(void) {
    if (s_naSock != INVALID_SOCKET) { closesocket(s_naSock); s_naSock = INVALID_SOCKET; }
}

void Ntp_AsyncCancel(void) {
    AsyncReleaseDns();
    AsyncReleaseSock();
    s_naState = NA_IDLE;
    s_naDeadline = 0;
}

int Ntp_AsyncBusy(void) { return (s_naState != NA_IDLE) ? 1 : 0; }

/* Send the request once DNS has produced an address. */
static int AsyncSend(unsigned long ip) {
    struct sockaddr_in sa;
    BYTE pkt[NTP_PKT_LEN];

    if (!ip) return 0;
    s_naSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_naSock == INVALID_SOCKET) return 0;

    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)NTP_PORT);
    sa.sin_addr.s_addr = ip;
    ZeroMemory(pkt, sizeof(pkt));
    pkt[0] = 0x1B;
    if (sendto(s_naSock, (const char*)pkt, NTP_PKT_LEN, 0,
        (struct sockaddr*)&sa, sizeof(sa)) != NTP_PKT_LEN) {
        AsyncReleaseSock();
        return 0;
    }
    s_naState = NA_RECV;
    s_naDeadline = GetTickCount() + NTP_RECV_MS;
    return 1;
}

/* Start DNS (or direct dotted-quad send) for the next usable server. */
static int AsyncStartNext(void) {
    while (k_servers[s_naServer]) {
        const char* host = k_servers[s_naServer];
        unsigned long ip = inet_addr(host);
        if (ip != INADDR_NONE) {
            if (AsyncSend(ip)) return 1;
            s_naServer++;
            continue;
        }

        s_naEvent = WSACreateEvent();
        s_naDns = NULL;
        if (XNetDnsLookup(host, s_naEvent, &s_naDns) == 0 && s_naDns) {
            s_naState = NA_DNS;
            s_naDeadline = GetTickCount() + NTP_DNS_MS;
            return 1;
        }
        AsyncReleaseDns();
        s_naServer++;
    }
    s_naState = NA_IDLE;
    return 0;
}

int Ntp_AsyncBegin(void) {
    if (Ntp_AsyncBusy()) return 1;
    if (!Net_IsUp()) return 0;
    s_naServer = 0;
    return AsyncStartNext();
}

int Ntp_AsyncPoll(void) {
    DWORD now;
    if (!Ntp_AsyncBusy()) return -1;
    now = GetTickCount();

    if (s_naState == NA_DNS) {
        if (s_naDns && s_naDns->iStatus == 0) {
            unsigned long ip = (s_naDns->cina > 0) ? s_naDns->aina[0].s_addr : 0;
            AsyncReleaseDns();
            if (ip && AsyncSend(ip)) return 0;
            s_naServer++;
            return AsyncStartNext() ? 0 : -1;
        }
        if (!s_naDns || s_naDns->iStatus != WSAEINPROGRESS || (LONG)(now - s_naDeadline) >= 0) {
            AsyncReleaseDns();
            s_naServer++;
            return AsyncStartNext() ? 0 : -1;
        }
        return 0;
    }

    if (s_naState == NA_RECV) {
        fd_set rfds;
        struct timeval tv;
        BYTE pkt[NTP_PKT_LEN];
        int got;
        unsigned long unixSecs;

        int ready;
        FD_ZERO(&rfds); FD_SET(s_naSock, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 0;              /* poll only; never block */
        ready = select(0, &rfds, NULL, NULL, &tv);
        if (ready > 0) {
            got = recv(s_naSock, (char*)pkt, NTP_PKT_LEN, 0);
            AsyncReleaseSock();
            if (PacketUnixSecs(pkt, got, &unixSecs) && ApplyUnixSecs(unixSecs)) {
                s_naState = NA_IDLE;
                return 1;
            }
            s_naServer++;
            return AsyncStartNext() ? 0 : -1;
        }
        if (ready < 0) {
            AsyncReleaseSock();
            s_naServer++;
            return AsyncStartNext() ? 0 : -1;
        }
        if ((LONG)(now - s_naDeadline) >= 0) {
            AsyncReleaseSock();
            s_naServer++;
            return AsyncStartNext() ? 0 : -1;
        }
        return 0;
    }

    Ntp_AsyncCancel();
    return -1;
}

int Ntp_Sync(void) {
    unsigned long unixSecs = 0;
    int got = 0, k;

    Ntp_AsyncCancel();                         /* manual sync owns NTP resources */
    if (!Net_IsUp()) return 0;                 /* need a working address */

    for (k = 0; k_servers[k]; k++) {
        if (QueryServer(k_servers[k], &unixSecs)) { got = 1; break; }
    }
    if (!got) return 0;

    /* Convert NTP UTC straight to system time and write it. NtSetSystemTime
       stores the clock in UTC; the Xbox kernel applies the EEPROM timezone bias
       when LOCAL time is read back for display (Sys_GetClock does exactly that).
       So we must write PURE UTC here and NOT pre-apply our own offset -- doing
       both is what double-counted the offset (showed 7h behind). The stored TZ
       offset is kept for reference/future use, but the live clock relies on the
       console's own timezone for local display. */
    return ApplyUnixSecs(unixSecs);
}