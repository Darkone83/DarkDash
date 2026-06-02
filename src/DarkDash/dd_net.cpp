/*---------------------------------------------------------------------------
    dd_net.cpp -- network bringup + IP, ported from XbDiag SysInfo.

    Net_Start() brings the stack up once (ref-counted XNetStartup, so it's safe
    regardless of what the dashboard left running) and kicks DHCP. Net_Poll()
    does a single non-blocking XNetGetTitleXnAddr -- no Sleep loop -- so it can
    be called from the main loop; the IP simply fills in once DHCP resolves.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <winsockx.h>
#include "xboxinternals.h"
#include "dd_net.h"

/* XNetConfigStatus / XNetConfigParams + their load/save/status externs now come
   from xboxinternals.h (winsockx.h included first for IN_ADDR). */

#define DDNET_V2_TAG        0x58425632u   /* "XBV2" */
#define DDNET_MANUAL_IP     0x00000004u
#define DDNET_MANUAL_DNS    0x00000008u

static char s_ip[24] = "No Link";
static char s_subnet[24] = "--";
static char s_gateway[24] = "--";
static char s_dns[24] = "--";
static int  s_up = 0;
static int  s_link = 0;        /* Ethernet cable/link active */
static int  s_started = 0;

/* append 0..255 as decimal, advancing *p */
static void AppendByte(char** p, int v) {
    char t[4];
    int  n = 0;
    if (v <= 0) { *(*p)++ = '0'; return; }
    while (v > 0 && n < 3) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) *(*p)++ = t[--n];
}

/* format a network-order IPv4 into "a.b.c.d" (param is NOT named s_addr --
   that's a winsock macro for S_un.S_addr and would expand inside the decl) */
static void FmtAddr(unsigned long addr, char* out) {
    BYTE* b = (BYTE*)&addr;
    char* p = out;
    if (addr == 0) { out[0] = '-'; out[1] = '-'; out[2] = 0; return; }
    AppendByte(&p, b[0]); *p++ = '.';
    AppendByte(&p, b[1]); *p++ = '.';
    AppendByte(&p, b[2]); *p++ = '.';
    AppendByte(&p, b[3]); *p = 0;
}

void Net_Start(void) {
    XNetStartupParams xnsp;
    WSADATA           wsa;
    if (s_started) return;

    /* Match XbDiag exactly: bare startup, cfgFlags only. Zeroed fields mean
       "system defaults", which are sufficient to host a listening server --
       XbDiag's FTP runs on this same config. */
    ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    XNetStartup(&xnsp);

    WSAStartup(MAKEWORD(2, 2), &wsa);
    s_started = 1;
}

void Net_Poll(void) {
    XNADDR xna;
    DWORD  st;
    if (!s_started) return;

    s_link = (XNetGetEthernetLinkStatus() & XNET_ETHERNET_LINK_ACTIVE) ? 1 : 0;

    ZeroMemory(&xna, sizeof(xna));
    st = XNetGetTitleXnAddr(&xna);
    if (st == XNET_GET_XNADDR_PENDING) return;             /* DHCP not done yet */

    if (!(st & XNET_GET_XNADDR_NONE) && xna.ina.s_addr != 0) {
        BYTE* b = (BYTE*)&xna.ina.s_addr;
        char* p = s_ip;
        AppendByte(&p, b[0]); *p++ = '.';
        AppendByte(&p, b[1]); *p++ = '.';
        AppendByte(&p, b[2]); *p++ = '.';
        AppendByte(&p, b[3]); *p = 0;
        s_up = 1;

        /* full config (subnet / gateway / DNS) once the stack is configured */
        {
            XNetConfigStatus cs;
            ZeroMemory(&cs, sizeof(cs));
            XNetGetConfigStatus(&cs);
            FmtAddr(cs.inaMask.S_un.S_addr, s_subnet);
            FmtAddr(cs.inaGateway.S_un.S_addr, s_gateway);
            FmtAddr(cs.inaDnsPrimary.S_un.S_addr, s_dns);
        }
    }
    /* else: leave the previous value ("No Link" until first success) */
}

const char* Net_Ip(void) { return s_ip; }
const char* Net_Subnet(void) { return s_subnet; }
const char* Net_Gateway(void) { return s_gateway; }
const char* Net_Dns(void) { return s_dns; }
int         Net_IsUp(void) { return s_up; }
int         Net_LinkUp(void) { return s_link; }

/*---------------------------------------------------------------------------
    Network configuration (mode + static IPs). Reads/writes the saved config
    block via XNetLoadConfigParams / XNetSaveConfigParams -- the supported API
    for the network-config EEPROM region (not the factory/video settings) --
    then restarts the stack so XNetStartup picks the new config up. Modelled on
    Team Resurgent's network::configure().

    Modes:  DD_NET_DHCP        Flag = 0            (DHCP assigns everything)
            DD_NET_STATIC      Flag = MANUAL_IP|MANUAL_DNS  (all manual)
            DD_NET_DHCP_DNS    Flag = MANUAL_DNS   (DHCP IP, manual DNS)
---------------------------------------------------------------------------*/

void Net_Restart(void) {
    if (s_started) {
        WSACleanup();
        XNetCleanup();
        s_started = 0;
    }
    s_ip[0] = 'N'; s_ip[1] = 'o'; s_ip[2] = ' '; s_ip[3] = 'L'; s_ip[4] = 'i';
    s_ip[5] = 'n'; s_ip[6] = 'k'; s_ip[7] = 0;
    s_up = 0;
    Net_Start();   /* re-startup: reads the freshly-saved config */
}

void Net_LoadConfig(int* mode, unsigned long* ip, unsigned long* mask,
    unsigned long* gw, unsigned long* dns1, unsigned long* dns2) {
    XNetConfigParams p;
    int v2;
    ZeroMemory(&p, sizeof(p));
    XNetLoadConfigParams(&p);
    v2 = (p.V2_Tag == DDNET_V2_TAG);

    *ip = v2 ? p.V2_IP : p.V1_IP;
    *mask = v2 ? p.V2_Subnetmask : p.V1_Subnetmask;
    *gw = v2 ? p.V2_Defaultgateway : p.V1_Defaultgateway;
    *dns1 = v2 ? p.V2_DNS1 : p.V1_DNS1;
    *dns2 = v2 ? p.V2_DNS2 : p.V1_DNS2;

    if (p.Flag & DDNET_MANUAL_IP)       *mode = DD_NET_STATIC;
    else if (p.Flag & DDNET_MANUAL_DNS) *mode = DD_NET_DHCP_DNS;
    else                                *mode = DD_NET_DHCP;
}

int Net_ApplyConfig(int mode, unsigned long ip, unsigned long mask,
    unsigned long gw, unsigned long dns1, unsigned long dns2) {
    XNetConfigParams p;
    int   v2;
    DWORD flag = 0;
    DWORD wantIp, wantMask, wantGw, wantDns1, wantDns2;

    ZeroMemory(&p, sizeof(p));
    if (XNetLoadConfigParams(&p) != 0) return 0;   /* couldn't read -> bail */
    v2 = (p.V2_Tag == DDNET_V2_TAG);

    if (mode == DD_NET_STATIC) {
        flag = DDNET_MANUAL_IP | DDNET_MANUAL_DNS;
        wantIp = ip; wantMask = mask; wantGw = gw; wantDns1 = dns1; wantDns2 = dns2;
    }
    else if (mode == DD_NET_DHCP_DNS) {
        flag = DDNET_MANUAL_DNS;
        wantIp = 0; wantMask = 0; wantGw = 0; wantDns1 = dns1; wantDns2 = dns2;
    }
    else { /* DHCP */
        flag = 0;
        wantIp = 0; wantMask = 0; wantGw = 0; wantDns1 = 0; wantDns2 = 0;
    }

    if (v2) {
        p.V2_IP = wantIp; p.V2_Subnetmask = wantMask; p.V2_Defaultgateway = wantGw;
        p.V2_DNS1 = wantDns1; p.V2_DNS2 = wantDns2;
    }
    else {
        p.V1_IP = wantIp; p.V1_Subnetmask = wantMask; p.V1_Defaultgateway = wantGw;
        p.V1_DNS1 = wantDns1; p.V1_DNS2 = wantDns2;
    }
    p.Flag = flag;

    if (XNetSaveConfigParams(&p) != 0) return 0;   /* EEPROM write refused */
    Net_Restart();
    return 1;
}