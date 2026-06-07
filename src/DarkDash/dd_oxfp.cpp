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

/* mode names for the UI; order/values match the firmware OXFP_Mode enum */
static const char* k_modeNames[OXFP_MODE_COUNT] = {
    "Stock", "Static", "Animation"
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

/* animation names; order/values match the firmware OXFP_AnimMode enum (0..10).
   Abbreviated to fit the value column: Bounce=ColorBounce, Breathe=Breathing,
   RGB Fade=RGBFade, Flicker=FireFlicker, Opposed=OpposedBreath. */
static const char* k_animNames[OXFP_ANIM_COUNT] = {
    "Bounce", "Breathe", "Chase", "RGB Fade", "Blink", "Alternate",
    "Flicker", "Plasma", "Heartbeat", "Opposed", "Sparkle"
};
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

/* ---- live config read-back ------------------------------------------------
   Oxfp_RequestConfig asks the firmware for its current state; the reply is
   captured by dd_udp. Oxfp_ParseConfig is a tiny read-only field scanner -- it
   finds "key": and reads the int or [r,g,b] that follows. Compact ArduinoJson
   output ("key":val, no spaces) is what we match; spaces are tolerated anyway.
   No JSON library, no sscanf. C89 style. */
int Oxfp_RequestConfig(void) {
    static const char k[] = "{\"op\":\"get\"}";
    if (!Oxfp_Present()) return 0;
    return Udp_QueryDevice(UDP_DEV_OXFP, k, (int)(sizeof(k) - 1));
}

/* return a pointer just past `"key":`, or 0. Matches the whole quoted key so
   "mode" never matches inside "animMode". */
static const char* JFind(const char* j, const char* key) {
    int kl = 0;
    while (key[kl]) kl++;
    while (*j) {
        if (*j == '"') {
            int i = 0;
            const char* q = j + 1;
            while (i < kl && q[i] == key[i]) i++;
            if (i == kl && q[i] == '"' && q[i + 1] == ':') return q + i + 2;
        }
        j++;
    }
    return 0;
}

static int JInt(const char* p, int* out) {
    int v = 0, neg = 0, any = 0;
    if (!p) return 0;
    while (*p == ' ') p++;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
    if (!any) return 0;
    *out = neg ? -v : v;
    return 1;
}

/* read [r,g,b] (each clamped to a byte) into out[3]. */
static int JRgb(const char* p, int out[3]) {
    int i, v, any;
    if (!p) return 0;
    while (*p == ' ') p++;
    if (*p != '[') return 0;
    p++;
    for (i = 0; i < 3; i++) {
        while (*p == ' ') p++;
        v = 0; any = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; }
        if (!any) return 0;
        out[i] = v & 0xFF;
        while (*p == ' ') p++;
        if (i < 2) { if (*p != ',') return 0; p++; }
    }
    return 1;
}

int Oxfp_ParseConfig(const char* json, int len, OxfpDevCfg* out) {
    const char* p;
    int v, got = 0;
    (void)len;
    if (!json || !out) return 0;
    out->mode = out->brightness = out->animMode = out->animSpeed = -1;
    out->green[0] = out->red[0] = out->orange[0] = -1;
    out->animA[0] = out->animB[0] = -1;
    if ((p = JFind(json, "mode")) != 0 && JInt(p, &v)) { out->mode = v; got = 1; }
    if ((p = JFind(json, "brightness")) != 0 && JInt(p, &v)) { out->brightness = v; got = 1; }
    if ((p = JFind(json, "animMode")) != 0 && JInt(p, &v)) { out->animMode = v; got = 1; }
    if ((p = JFind(json, "animSpeed")) != 0 && JInt(p, &v)) { out->animSpeed = v; got = 1; }
    if ((p = JFind(json, "greenColor")) != 0 && JRgb(p, out->green)) got = 1;
    if ((p = JFind(json, "redColor")) != 0 && JRgb(p, out->red)) got = 1;
    if ((p = JFind(json, "orangeColor")) != 0 && JRgb(p, out->orange)) got = 1;
    if ((p = JFind(json, "animColorA")) != 0 && JRgb(p, out->animA)) got = 1;
    if ((p = JFind(json, "animColorB")) != 0 && JRgb(p, out->animB)) got = 1;
    return got;
}