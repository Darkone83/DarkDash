/*---------------------------------------------------------------------------
    dd_ntp.cpp -- see dd_ntp.h.

    Minimal SNTP client. Sends a 48-byte client packet, reads the server reply,
    pulls the transmit timestamp (seconds since 1900-01-01) from bytes 40..43,
    converts NTP epoch -> Win32 FILETIME (100ns since 1601), applies the stored
    TZ offset to get local wall time, and writes it to the RTC.

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
#define NTP_RECV_MS     3000   /* per-server receive timeout */

/* seconds between 1900-01-01 (NTP epoch) and 1970-01-01 (Unix) */
#define NTP_UNIX_DELTA  2208988800UL

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

    /* transmit timestamp seconds: bytes 40..43, big-endian, since 1900 */
    {
        unsigned long ntpSecs =
            ((unsigned long)pkt[40] << 24) | ((unsigned long)pkt[41] << 16) |
            ((unsigned long)pkt[42] << 8) | (unsigned long)pkt[43];
        if (ntpSecs <= NTP_UNIX_DELTA) return 0;     /* sanity */
        *unixSecs = ntpSecs - NTP_UNIX_DELTA;
        return 1;
    }
}

int Ntp_Sync(void) {
    unsigned long unixSecs = 0;
    int got = 0, k;

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
    {
        ULONGLONG ft;
        FILETIME  filetime;
        SYSTEMTIME stime;
        SysClock  c;

        ft = ((ULONGLONG)unixSecs + 11644473600ULL) * 10000000ULL; /* 1601->1970 */
        filetime.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFULL);
        filetime.dwHighDateTime = (DWORD)(ft >> 32);

        if (!FileTimeToSystemTime(&filetime, &stime)) return 0;

        /* UTC fields -> Sys_SetClockDirect writes them as-is via NtSetSystemTime
           (which stores UTC). The kernel applies the EEPROM TZ on read-back for
           local display, so the wall clock comes out correct. */
        c.year = stime.wYear; c.mon = stime.wMonth; c.day = stime.wDay;
        c.hour = stime.wHour; c.min = stime.wMinute; c.sec = stime.wSecond;
        c.dow = stime.wDayOfWeek;
        return Sys_SetClockDirect(&c);
    }
}