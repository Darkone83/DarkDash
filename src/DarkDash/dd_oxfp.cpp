/*---------------------------------------------------------------------------
    dd_oxfp.cpp -- see dd_oxfp.h.

    Builds the small JSON control packets the OXFP firmware accepts and sends
    them to the device (unicast to its last-known address, else broadcast) via
    dd_udp. We never need a JSON parser here -- we only emit, with tiny hand
    -rolled number formatting (no sprintf). All ops are gated on presence so a
    missing OXFP just means a no-op. C89 style, file-scope statics.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_oxfp.h"
#include "dd_udp.h"

/* mode names for the UI; keep in sync with the firmware OXFP_Mode order */
static const char* k_modeNames[OXFP_MODE_COUNT] = {
    "Auto", "Solid", "Animated", "Off"
};

const char* Oxfp_ModeName(int mode) {
    if (mode < 0 || mode >= OXFP_MODE_COUNT) return "?";
    return k_modeNames[mode];
}

void Oxfp_Init(void) { /* nothing yet; discovery lives in dd_udp */ }

int Oxfp_Present(void) { return Udp_Present(UDP_DEV_OXFP); }

/* append an integer to buf at *pos (no sprintf); returns updated pos */
static int AppendInt(char* buf, int pos, int cap, int v) {
    char t[12]; int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    if (neg && pos < cap - 1) buf[pos++] = '-';
    while (n && pos < cap - 1) buf[pos++] = t[--n];
    return pos;
}

static int AppendStr(char* buf, int pos, int cap, const char* s) {
    while (*s && pos < cap - 1) buf[pos++] = *s++;
    return pos;
}

int Oxfp_SetMode(int mode) {
    char pkt[64]; int p = 0;
    if (!Oxfp_Present()) return 0;
    p = AppendStr(pkt, p, sizeof(pkt), "{\"op\":\"mode\",\"mode\":");
    p = AppendInt(pkt, p, sizeof(pkt), mode);
    p = AppendStr(pkt, p, sizeof(pkt), "}");
    pkt[p] = 0;
    return Udp_SendToDevice(UDP_DEV_OXFP, pkt, p);
}

int Oxfp_SetBrightness(int v) {
    char pkt[64]; int p = 0;
    if (!Oxfp_Present()) return 0;
    if (v < 0) v = 0; if (v > 255) v = 255;
    /* "set" applies live (firmware treats it as a long preview) */
    p = AppendStr(pkt, p, sizeof(pkt), "{\"op\":\"set\",\"brightness\":");
    p = AppendInt(pkt, p, sizeof(pkt), v);
    p = AppendStr(pkt, p, sizeof(pkt), "}");
    pkt[p] = 0;
    return Udp_SendToDevice(UDP_DEV_OXFP, pkt, p);
}

int Oxfp_Identify(int ms) {
    char pkt[64]; int p = 0;
    if (!Oxfp_Present()) return 0;
    if (ms <= 0) ms = 1500;
    p = AppendStr(pkt, p, sizeof(pkt), "{\"op\":\"identify\",\"ms\":");
    p = AppendInt(pkt, p, sizeof(pkt), ms);
    p = AppendStr(pkt, p, sizeof(pkt), "}");
    pkt[p] = 0;
    return Udp_SendToDevice(UDP_DEV_OXFP, pkt, p);
}

int Oxfp_Save(void) {
    static const char k[] = "{\"op\":\"save\"}";
    if (!Oxfp_Present()) return 0;
    return Udp_SendToDevice(UDP_DEV_OXFP, k, (int)(sizeof(k) - 1));
}

/* emit {"op":"set","KEY":[r,g,b]} for an [r,g,b] color field */
static int SendColor(const char* key, unsigned long rgb) {
    char pkt[80]; int p = 0;
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    if (!Oxfp_Present()) return 0;
    p = AppendStr(pkt, p, sizeof(pkt), "{\"op\":\"set\",\"");
    p = AppendStr(pkt, p, sizeof(pkt), key);
    p = AppendStr(pkt, p, sizeof(pkt), "\":[");
    p = AppendInt(pkt, p, sizeof(pkt), r); p = AppendStr(pkt, p, sizeof(pkt), ",");
    p = AppendInt(pkt, p, sizeof(pkt), g); p = AppendStr(pkt, p, sizeof(pkt), ",");
    p = AppendInt(pkt, p, sizeof(pkt), b);
    p = AppendStr(pkt, p, sizeof(pkt), "]}");
    pkt[p] = 0;
    return Udp_SendToDevice(UDP_DEV_OXFP, pkt, p);
}

int Oxfp_SetStatusColor(int which, unsigned long rgb) {
    static const char* k_key[OXFP_COL_COUNT] = { "greenColor", "redColor", "orangeColor" };
    if (which < 0 || which >= OXFP_COL_COUNT) return 0;
    return SendColor(k_key[which], rgb);
}

int Oxfp_SetAnimColor(int ab, unsigned long rgb) {
    return SendColor(ab ? "animColorB" : "animColorA", rgb);
}

static const char* k_animNames[OXFP_ANIM_COUNT] = { "Static", "Pulse", "Cycle", "Sweep" };
const char* Oxfp_AnimName(int anim) {
    if (anim < 0 || anim >= OXFP_ANIM_COUNT) return "?";
    return k_animNames[anim];
}

int Oxfp_SetAnimMode(int anim) {
    char pkt[48]; int p = 0;
    if (!Oxfp_Present()) return 0;
    p = AppendStr(pkt, p, sizeof(pkt), "{\"op\":\"set\",\"animMode\":");
    p = AppendInt(pkt, p, sizeof(pkt), anim);
    p = AppendStr(pkt, p, sizeof(pkt), "}");
    pkt[p] = 0;
    return Udp_SendToDevice(UDP_DEV_OXFP, pkt, p);
}

int Oxfp_SetAnimSpeed(int v) {
    char pkt[48]; int p = 0;
    if (!Oxfp_Present()) return 0;
    if (v < 0) v = 0; if (v > 255) v = 255;
    p = AppendStr(pkt, p, sizeof(pkt), "{\"op\":\"set\",\"animSpeed\":");
    p = AppendInt(pkt, p, sizeof(pkt), v);
    p = AppendStr(pkt, p, sizeof(pkt), "}");
    pkt[p] = 0;
    return Udp_SendToDevice(UDP_DEV_OXFP, pkt, p);
}

int Oxfp_Reset(void) {
    static const char k[] = "{\"op\":\"reset\"}";
    if (!Oxfp_Present()) return 0;
    return Udp_SendToDevice(UDP_DEV_OXFP, k, (int)(sizeof(k) - 1));
}