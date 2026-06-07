/*---------------------------------------------------------------------------
    dd_rgb.cpp -- see dd_rgb.h.

    Emits the firmware's preview/save ops with a "cfg" object holding the fields
    we change. The firmware applies partial cfg (only keys present are touched),
    so a single-field set is fine. Live changes use op "preview"; persisted ones
    use op "save". Hand-rolled JSON, no parser, no sprintf. C89 style.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_rgb.h"
#include "dd_udp.h"

static const char* k_modeNames[RGB_MODE_COUNT] = {
    "Solid", "Breathe", "Color Wipe", "Larson", "Rainbow", "Theater",
    "Twinkle", "Comet", "Meteor", "Clock Spin", "Plasma", "Fire",
    "Palette Cycle", "Palette Chase", "Custom", "UNSC/Covenant"
};

const char* Rgb_ModeName(int mode) {
    if (mode < 0 || mode >= RGB_MODE_COUNT) return "?";
    return k_modeNames[mode];
}

void Rgb_Init(void) { /* discovery lives in dd_udp (passive listen) */ }

int Rgb_Present(void) { return Udp_Present(UDP_DEV_RGB); }

static int AppendStr(char* b, int p, int cap, const char* s) {
    while (*s && p < cap - 1) b[p++] = *s++;
    return p;
}
static int AppendInt(char* b, int p, int cap, long v) {
    char t[16]; int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    if (neg && p < cap - 1) b[p++] = '-';
    while (n && p < cap - 1) b[p++] = t[--n];
    return p;
}

/* Build {"op":OP,"cfg":{KEY:VAL}} and send. op = "save" or "preview". */
static int SendCfgInt(const char* key, long val, int save) {
    char pkt[96]; int p = 0;
    if (!Rgb_Present()) return 0;
    p = AppendStr(pkt, p, sizeof(pkt), save ? "{\"op\":\"save\",\"cfg\":{\""
        : "{\"op\":\"preview\",\"cfg\":{\"");
    p = AppendStr(pkt, p, sizeof(pkt), key);
    p = AppendStr(pkt, p, sizeof(pkt), "\":");
    p = AppendInt(pkt, p, sizeof(pkt), val);
    p = AppendStr(pkt, p, sizeof(pkt), "}}");
    pkt[p] = 0;
    return Udp_SendToDevice(UDP_DEV_RGB, pkt, p);
}

int Rgb_SetMode(int mode, int save) {
    if (mode < 0) mode = 0;
    if (mode >= RGB_MODE_COUNT) mode = RGB_MODE_COUNT - 1;
    return SendCfgInt("mode", mode, save);
}

int Rgb_SetBrightness(int v, int save) {
    if (v < 0) v = 0; if (v > 255) v = 255;
    return SendCfgInt("brightness", v, save);
}

int Rgb_SetSpeed(int v, int save) {
    if (v < 0) v = 0; if (v > 255) v = 255;
    return SendCfgInt("speed", v, save);
}

int Rgb_SetIntensity(int v, int save) {
    if (v < 0) v = 0; if (v > 255) v = 255;
    return SendCfgInt("intensity", v, save);
}

int Rgb_SetPaletteCount(int n, int save) {
    if (n < 1) n = 1; if (n > 4) n = 4;
    return SendCfgInt("paletteCount", n, save);
}

int Rgb_SetColor(int slot, unsigned long rgb, int save) {
    static const char* k_key[4] = { "colorA", "colorB", "colorC", "colorD" };
    if (slot < 0 || slot > 3) return 0;
    return SendCfgInt(k_key[slot], (long)(rgb & 0xFFFFFFUL), save);
}

int Rgb_SetColorA(unsigned long rgb, int save) {
    return Rgb_SetColor(0, rgb, save);
}

/* per-mode color usage. Mirrors the firmware anim functions: most modes read
   colorA only; ClockSpin uses A+B; the palette/Halo modes use paletteCount of
   A..D; Rainbow/Plasma/Fire/Custom are generated. Validate on hardware. */
int Rgb_ModeColorCount(int mode) {
    static const signed char k_cnt[RGB_MODE_COUNT] = {
        1, /* Solid        */  1, /* Breathe      */  1, /* Color Wipe */
        1, /* Larson       */  0, /* Rainbow(gen) */  1, /* Theater    */
        1, /* Twinkle      */  1, /* Comet        */  1, /* Meteor     */
        2, /* Clock Spin   */  0, /* Plasma(gen)  */  0, /* Fire(gen)  */
       -1, /* Palette Cycle*/ -1, /* Palette Chase*/  0, /* Custom     */
       -1  /* UNSC/Covenant*/
    };
    if (mode < 0 || mode >= RGB_MODE_COUNT) return 0;
    return (int)k_cnt[mode];
}

int Rgb_Reset(void) {
    static const char k[] = "{\"op\":\"reset\"}";
    if (!Rgb_Present()) return 0;
    return Udp_SendToDevice(UDP_DEV_RGB, k, (int)(sizeof(k) - 1));
}

/* ---- live config read-back ------------------------------------------------
   Rgb_RequestConfig asks the firmware for its current config; the reply (the
   "cfg" object) is captured by dd_udp. Rgb_ParseConfig scans for "key": and
   reads the int that follows -- colors are packed 0xRRGGBB ints here (unlike
   OXFP's [r,g,b]). Nesting under "cfg" is irrelevant to a flat key scan since
   every key we read is RGB-unique. No JSON library, no sscanf. C89 style. */
int Rgb_RequestConfig(void) {
    static const char k[] = "{\"op\":\"get\"}";
    if (!Rgb_Present()) return 0;
    return Udp_QueryDevice(UDP_DEV_RGB, k, (int)(sizeof(k) - 1));
}

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

static int JLong(const char* p, long* out) {
    long v = 0;
    int neg = 0, any = 0;
    if (!p) return 0;
    while (*p == ' ') p++;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (long)(*p - '0'); p++; any = 1; }
    if (!any) return 0;
    *out = neg ? -v : v;
    return 1;
}

int Rgb_ParseConfig(const char* json, int len, RgbDevCfg* out) {
    const char* p;
    long v;
    int got = 0;
    (void)len;
    if (!json || !out) return 0;
    out->mode = out->brightness = out->speed = out->intensity = out->paletteCount = -1;
    out->colorA = out->colorB = out->colorC = out->colorD = -1;
    if ((p = JFind(json, "mode")) != 0 && JLong(p, &v)) { out->mode = (int)v; got = 1; }
    if ((p = JFind(json, "brightness")) != 0 && JLong(p, &v)) { out->brightness = (int)v; got = 1; }
    if ((p = JFind(json, "speed")) != 0 && JLong(p, &v)) { out->speed = (int)v; got = 1; }
    if ((p = JFind(json, "intensity")) != 0 && JLong(p, &v)) { out->intensity = (int)v; got = 1; }
    if ((p = JFind(json, "paletteCount")) != 0 && JLong(p, &v)) { out->paletteCount = (int)v; got = 1; }
    if ((p = JFind(json, "colorA")) != 0 && JLong(p, &v)) { out->colorA = v & 0xFFFFFFL; got = 1; }
    if ((p = JFind(json, "colorB")) != 0 && JLong(p, &v)) { out->colorB = v & 0xFFFFFFL; got = 1; }
    if ((p = JFind(json, "colorC")) != 0 && JLong(p, &v)) { out->colorC = v & 0xFFFFFFL; got = 1; }
    if ((p = JFind(json, "colorD")) != 0 && JLong(p, &v)) { out->colorD = v & 0xFFFFFFL; got = 1; }
    return got;
}