/*---------------------------------------------------------------------------
    dd_typed.cpp -- see dd_typed.h.

    Broadcasts the Type-D "core" telemetry packet on UDP 50504. The wire format
    must match the Type-D receiver's CorePacket exactly:
        struct CorePacket {
            int32_t fanSpeed;
            int32_t cpuTemp;
            int32_t ambientTemp;
            char    currentApp[32];
        };  // 3*4 + 32 = 44 bytes, no padding
    We use 'long' (4 bytes, MSVC/Xbox) for the int32 fields and assemble the
    bytes explicitly so layout/endianness is unambiguous (x86 is little-endian,
    matching the ESP32). C89 style, file-scope statics.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_typed.h"
#include "dd_udp.h"
#include "dd_dc.h"
#include "dd_sysinfo.h"
#include "dd_launcher.h"

#define TYPED_PORT       50504
#define TYPED_APP_LEN    32
#define TYPED_PKT_LEN    (3 * 4 + TYPED_APP_LEN)   /* 44 */
#define TYPED_SEND_MS    1000                       /* broadcast ~1/sec */

static DWORD s_lastSend = 0;
static int   s_started = 0;

/* The app name DarkDash reports: the currently highlighted title if we're in a
   launcher list (resolved as resource-pack -> XBE cert -> folder name), else the
   dashboard itself. */
static const char* k_fallbackName = "DarkDash";

static const char* CurrentAppName(void) {
    const char* n = Launcher_CurrentAppName();
    return (n && n[0]) ? n : k_fallbackName;
}

static void PutI32LE(BYTE* p, long v) {
    p[0] = (BYTE)(v & 0xFF);
    p[1] = (BYTE)((v >> 8) & 0xFF);
    p[2] = (BYTE)((v >> 16) & 0xFF);
    p[3] = (BYTE)((v >> 24) & 0xFF);
}

void TypeD_Init(void) {
    Dc_Load();
    s_started = 1;
    s_lastSend = 0;
}

void TypeD_Tick(void) {
    BYTE pkt[TYPED_PKT_LEN];
    int  cpuC = 0, boardC = 0, fanPct = 0;
    DWORD now;
    int i;
    const char* app;
    static char s_lastApp[TYPED_APP_LEN] = { 0 };
    int appChanged;

    if (!s_started) return;
    if (!Dc_TypeDEnabled()) return;

    /* Detect a change in the reported app name. The telemetry (fan/temps) only
       needs a ~1/sec refresh, but the app name should be pushed the instant the
       highlighted title changes so Type-D tracks the selection promptly instead
       of lagging up to a second behind. */
    app = CurrentAppName();
    appChanged = 0;
    for (i = 0; i < TYPED_APP_LEN - 1; i++) {
        char a = app[i], b = s_lastApp[i];
        if (a != b) { appChanged = 1; break; }
        if (a == 0) break;
    }

    now = GetTickCount();
    if (!appChanged && s_lastSend != 0 && (now - s_lastSend) < TYPED_SEND_MS) return;
    s_lastSend = now;

    /* remember what we sent so the next change is detected */
    for (i = 0; i < TYPED_APP_LEN - 1 && app[i]; i++) s_lastApp[i] = app[i];
    s_lastApp[i] = 0;

    /* live readings (best-effort; 0 if unavailable) */
    if (!Sys_ReadTemps(&cpuC, &boardC)) { cpuC = 0; boardC = 0; }
    if (!Sys_ReadFanPct(&fanPct)) { fanPct = 0; }

    /* assemble CorePacket: fanSpeed, cpuTemp, ambientTemp, currentApp[32] */
    {
        PutI32LE(pkt + 0, (long)fanPct);
        PutI32LE(pkt + 4, (long)cpuC);
        PutI32LE(pkt + 8, (long)boardC);   /* board temp as "ambient" */
        for (i = 0; i < TYPED_APP_LEN; i++) pkt[12 + i] = 0;
        for (i = 0; i < TYPED_APP_LEN - 1 && app[i]; i++) pkt[12 + i] = (BYTE)app[i];
    }

    Udp_Broadcast(TYPED_PORT, pkt, TYPED_PKT_LEN);
}

int  TypeD_Enabled(void) { return Dc_TypeDEnabled(); }
void TypeD_SetEnabled(int on) { Dc_SetTypeDEnabled(on); }