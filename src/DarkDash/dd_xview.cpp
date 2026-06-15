/*---------------------------------------------------------------------------
    dd_xview.cpp -- see dd_xview.h.

    Panel thread flow (mirrors INTEGRATION.md s5):
      warm-up ~2s  -> XvXbox_Init -> Ping/QueryInfo -> manual present
      -> clear -> branded splash -> live data pages over plasma -> outro

    We own every pixel: a themed palette plasma is composited with our own
    bitmap font (transparent backgrounds, drop shadows) into a full-res
    framebuffer and pushed with one Blit per frame. Integer-only (no float /
    __ftol2_sse) so it is safe on this service thread.

    Build: MSVC2003 / C89 style; /GL safe.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_xview.h"
#include "xv_xbox.h"
#include "xv_client.h"
#include "dd_theme.h"
#include "dd_sysinfo.h"
#include "dd_net.h"
#include "dd_ftp.h"
#include "dd_lcd.h"
#include "settings.h"
#include "dd_launcher.h"
#include "dd_stbi.h"
#include "dd_xv_font.h"

/* ---- config (xview.dat) ----------------------------------------------- */

#define XVDAT_MAGIC   0x31775658u   /* 'X','V','w','1' */
#define XVDAT_VER     3
#define XVDAT_FILE    "D:\\data\\xview.dat"
#define XVDAT_DIR     "D:\\data"

typedef struct {
    DWORD magic;
    DWORD version;
    int   enable;
    int   brightness;   /* 0..255 */
    int   pages;        /* LCD_PAGE_* bitmask -- X-View's OWN page set (v2)   */
    int   intervalMs;   /* page rotation interval, ms (v2)                    */
    int   panel;        /* XV_PANEL_A / XV_PANEL_B preset (v3)                */
} XvCfg;

static XvCfg s_cfg;
static int   s_cfgLoaded = 0;

static void CfgDefaults(void) {
    s_cfg.magic = XVDAT_MAGIC; s_cfg.version = XVDAT_VER;
    s_cfg.enable = 1; s_cfg.brightness = 200;
    s_cfg.pages = LCD_PAGE_ALL; s_cfg.intervalMs = 4000; s_cfg.panel = XV_PANEL_B;
}

static void CfgLoad(void) {
    HANDLE h; DWORD got = 0;
    s_cfgLoaded = 1;
    CfgDefaults();
    h = CreateFileA(XVDAT_FILE, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        XvCfg tmp;
        if (ReadFile(h, &tmp, sizeof(tmp), &got, NULL) && got >= 16 &&
            tmp.magic == XVDAT_MAGIC) {
            s_cfg.enable = tmp.enable;
            s_cfg.brightness = tmp.brightness;
            if (s_cfg.brightness < 0)   s_cfg.brightness = 0;
            if (s_cfg.brightness > 255) s_cfg.brightness = 255;
            /* v2 appended pages + interval; older (v1) files keep the defaults
               set above so upgrading never wipes them. */
            if (tmp.version >= 2 && got >= (DWORD)sizeof(XvCfg)) {
                s_cfg.pages = tmp.pages ? tmp.pages : LCD_PAGE_ALL;
                s_cfg.intervalMs = tmp.intervalMs;
                if (tmp.version >= 3 && got >= (DWORD)sizeof(XvCfg))
                    s_cfg.panel = tmp.panel;
                if (s_cfg.intervalMs < 1000)  s_cfg.intervalMs = 4000;
                if (s_cfg.intervalMs > 30000) s_cfg.intervalMs = 30000;
            }
        }
        CloseHandle(h);
    }
}

static void CfgSave(void) {
    HANDLE h; DWORD wrote = 0;
    CreateDirectoryA(XVDAT_DIR, NULL);
    h = CreateFileA(XVDAT_FILE, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, &s_cfg, sizeof(s_cfg), &wrote, NULL);
        CloseHandle(h);
    }
}

/* ---- theme snapshot (written by main thread, read by panel thread) ----- */

typedef struct { int r, g, b; } Rgb;
static Rgb s_bg = { 6, 10, 8 };
static Rgb s_acc = { 174, 255, 60 };
static Rgb s_text = { 216, 248, 192 };
static Rgb s_dim = { 127, 160, 96 };
static int s_palDirty = 1;   /* rebuild the plasma palette on next frame */

static Rgb Unpack(DWORD argb) {
    Rgb c;
    c.r = (int)((argb >> 16) & 0xFF);
    c.g = (int)((argb >> 8) & 0xFF);
    c.b = (int)(argb & 0xFF);
    return c;
}

void XView_RefreshTheme(void) {
    s_bg = Unpack(Theme_Color("bg", 0xFF060A08));
    s_acc = Unpack(Theme_Color("accent", 0xFFAEFF3C));
    s_text = Unpack(Theme_Color("text", 0xFFD8F8C0));
    s_dim = Unpack(Theme_Color("text_dim", 0xFF7FA060));
    s_palDirty = 1;
}

/* ---- panel geometry --------------------------------------------------- */

static XvInfo   s_info;
static int      s_haveInfo = 0;
#define XV_ART_DIM 200
static uint16_t s_art[XV_ART_DIM * XV_ART_DIM];
static LONG s_ready = 0;

/* ---- thread state ------------------------------------------------------ */

static HANDLE s_thread = NULL;
static LONG   s_stop = 0;
static LONG   s_npMode = 0;        /* 1 = freeze a Now Playing art frame on stop */
static LONG   s_brightDirty = 0;   /* 1 = push brightness to the live panel      */
static LONG   s_panelDirty = 0;   /* 1 = re-send panel preset + re-read geometry */
static char   s_npTitle[64];
static char   s_npXbe[260];

#define XV_WARMUP_MS    2000
#define XV_RESCAN_MS    4000        /* hotplug: poll for the panel this often */
#define XV_STOP_WAIT_MS 5000        /* outro/shutdown + possible unplug teardown */
#define XV_NP_WAIT_MS   5000        /* art load + blit can take ~1-2s     */

static int Stopping(void) { return InterlockedCompareExchange(&s_stop, 0, 0) != 0; }

/* sleep in small slices so a stop request is honoured promptly */
static int SleepChecked(int ms) {
    int left = ms;
    while (left > 0) {
        if (Stopping()) return 1;
        Sleep(left > 50 ? 50 : left);
        left -= 50;
    }
    return Stopping();
}

/* ---- rendering -------------------------------------------------------- */

/* We own every pixel: a themed plasma is rendered into a full-res framebuffer,
   then our own font is composited on top with transparent backgrounds (only the
   glyph pixels are written), so the living plasma shows through the text. One
   Blit per frame pushes the whole 320x240 frame to the panel. */

#define XV_W 160      /* render buffer max width  (panel/2) */
#define XV_H 120         /* render buffer max height (panel/2) */
static uint16_t s_fb[XV_W * XV_H];

/* active render geometry -- always half-res, firmware upscales by s_scale.
   Set from INFO after a panel select; bounded by the buffer max (XV_W/XV_H).
   Type B 320x240 -> 160x120 x2; Type A 284x76 bar -> 142x38 x2. */
static int s_rw = 160, s_rh = 120, s_scale = 2;
static void XvSetGeom(int w, int h) {
    s_scale = 2;
    s_rw = w / s_scale; s_rh = h / s_scale;
    if (s_rw < 1) s_rw = 1; if (s_rw > XV_W) s_rw = XV_W;
    if (s_rh < 1) s_rh = 1; if (s_rh > XV_H) s_rh = XV_H;
}

/* parabolic integer sine LUT, -255..255, period 256 (no float on this thread) */
static int ISin(int i) {
    int x, y;
    i &= 255;
    if (i < 128) { x = i;       y = (4 * 255 * x * (128 - x)) / 16384; }
    else { x = i - 128; y = -(4 * 255 * x * (128 - x)) / 16384; }
    return y;
}

/* ---- themed plasma palette ------------------------------------------- */

#define XV_PALN 256                 /* power of two -> mask-wrap the index */
static uint16_t s_pal[XV_PALN];

static void BuildPalette(void) {
    int i; Rgb br;
    /* a bright highlight = accent pushed toward text/white */
    br.r = (s_acc.r + s_text.r + 255) / 3;
    br.g = (s_acc.g + s_text.g + 255) / 3;
    br.b = (s_acc.b + s_text.b + 255) / 3;
    for (i = 0; i < XV_PALN; i++) {
        int seg = i / (XV_PALN / 4);                 /* 0..3 */
        int f = (i % (XV_PALN / 4)) * 256 / (XV_PALN / 4);  /* 0..255 */
        Rgb a, b2; int r, g, bl;
        if (seg == 0) { a = s_bg;  b2 = s_acc; }   /* bg   -> accent */
        else if (seg == 1) { a = s_acc; b2 = br; }   /* accent -> bright */
        else if (seg == 2) { a = br;    b2 = s_acc; }   /* bright -> accent */
        else { a = s_acc; b2 = s_bg; }   /* accent -> bg    */
        r = a.r + (b2.r - a.r) * f / 256;
        g = a.g + (b2.g - a.g) * f / 256;
        bl = a.b + (b2.b - a.b) * f / 256;
        s_pal[i] = XvCli_Rgb565(r, g, bl);
    }
    s_palDirty = 0;
}

/* multi-octave plasma into s_fb; spatial motion + palette cycling via t */
static void RenderPlasma(int t) {
    int x, y;
    for (y = 0; y < s_rh; y++) {
        int yt = ISin(y * 2 + t);
        uint16_t* dst = s_fb + y * s_rw;
        for (x = 0; x < s_rw; x++) {
            int v = ISin(x * 2 - t)
                + yt
                + ISin((x + y) + (t >> 1))
                + ISin(((x - y) * 3) / 2 - (t >> 2));
            int idx = (((v + 1020) * (XV_PALN - 1)) / 2040 + (t >> 1)) & (XV_PALN - 1);
            dst[x] = s_pal[idx];
        }
    }
}

/* ---- framebuffer primitives ------------------------------------------ */

/* multiply a region's brightness by keep/256 (translucent dark card look) */
static void DarkenBand(int x0, int y0, int w, int h, int keep) {
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    for (y = y0; y < y0 + h && y < s_rh; y++) {
        uint16_t* dst = s_fb + y * s_rw;
        for (x = x0; x < x0 + w && x < s_rw; x++) {
            uint16_t c = dst[x];
            int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            r = r * keep / 256; g = g * keep / 256; b = b * keep / 256;
            dst[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static void FbRect(int x0, int y0, int w, int h, uint16_t c) {
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    for (y = y0; y < y0 + h && y < s_rh; y++) {
        uint16_t* dst = s_fb + y * s_rw;
        for (x = x0; x < x0 + w && x < s_rw; x++) dst[x] = c;
    }
}

static void FbBlitImg(int x0, int y0, const uint16_t* img, int w, int h) {
    int x, y;
    for (y = 0; y < h; y++) {
        int py = y0 + y; uint16_t* dst;
        if (py < 0 || py >= s_rh) continue;
        dst = s_fb + py * s_rw;
        for (x = 0; x < w; x++) {
            int px = x0 + x;
            if (px >= 0 && px < s_rw) dst[px] = img[y * w + x];
        }
    }
}

/* one glyph, scaled, transparent background (only set bits are drawn) */
static void FbGlyph(int x, int y, int sc, uint16_t col, int ch) {
    const unsigned char* g; int row, bx, cx, cy;
    if (ch < XVFONT_FIRST || ch > XVFONT_LAST) ch = '?';
    g = s_xvfont[ch - XVFONT_FIRST];
    for (row = 0; row < XVFONT_H; row++) {
        unsigned char bits = g[row];
        if (!bits) continue;
        for (bx = 0; bx < XVFONT_W; bx++) {
            if (!(bits & (1 << (7 - bx)))) continue;
            for (cy = 0; cy < sc; cy++) {
                int py = y + row * sc + cy; uint16_t* dst;
                if (py < 0 || py >= s_rh) continue;
                dst = s_fb + py * s_rw;
                for (cx = 0; cx < sc; cx++) {
                    int px = x + bx * sc + cx;
                    if (px >= 0 && px < s_rw) dst[px] = col;
                }
            }
        }
    }
}

/* monospace string; shadow=1 draws a 1px dark drop-shadow for legibility */
static void FbText(int x, int y, int sc, uint16_t col, int shadow, const char* s) {
    int adv = XVFONT_W * sc, xx;
    if (!s) return;
    if (shadow) {
        const char* p = s; uint16_t sh = XvCli_Rgb565(0, 0, 0);
        for (xx = x; *p; p++, xx += adv) FbGlyph(xx + 1, y + 1, sc, sh, (unsigned char)*p);
    }
    for (xx = x; *s; s++, xx += adv) FbGlyph(xx, y, sc, col, (unsigned char)*s);
}

/* small non-negative int -> decimal; returns length */
static int XvU2A(int v, char* out) {
    char tmp[12]; int n = 0, i = 0;
    if (v < 0) v = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return 1; }
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

/* compact size "4.2G" / "512M" into out; returns chars written */
static int XvSizeC(char* out, DWORD mb) {
    char num[12]; int p = 0, k, len;
    if (mb < 1024) {
        len = XvU2A((int)mb, num);
        for (k = 0; k < len; k++) out[p++] = num[k];
        out[p++] = 'M';
    }
    else {
        DWORD gb = mb / 1024, dec = (mb % 1024) * 10 / 1024;
        len = XvU2A((int)gb, num);
        for (k = 0; k < len; k++) out[p++] = num[k];
        out[p++] = '.'; out[p++] = (char)('0' + (int)dec); out[p++] = 'G';
    }
    out[p] = 0;
    return p;
}

/* ---- page content cache (rebuilt ~1/sec, drawn every frame) ----------- */

typedef struct { char label[14]; char val[26]; } XvKV;
static char  s_pgTitle[20];
static XvKV  s_pgLines[8];
static int   s_pgLineN;
static int   s_pgHasBar;
static unsigned long s_pgBarDone, s_pgBarTotal;

static void KV(int* n, const char* label, const char* val) {
    if (*n >= 8) return;
    lstrcpynA(s_pgLines[*n].label, label, (int)sizeof(s_pgLines[0].label));
    lstrcpynA(s_pgLines[*n].val, val, (int)sizeof(s_pgLines[0].val));
    (*n)++;
}

static void BuildContent(int bit) {
    int n = 0;
    s_pgHasBar = 0;
    switch (bit) {
    case LCD_PAGE_TEMPS: {
        char v[16]; int cpu = 0, brd = 0, fan = 0, e;
        lstrcpynA(s_pgTitle, "Temps", sizeof(s_pgTitle));
        if (Sys_ReadTemps(&cpu, &brd)) {
            e = XvU2A(cpu, v); v[e] = 'C'; v[e + 1] = 0; KV(&n, "CPU", v);
            e = XvU2A(brd, v); v[e] = 'C'; v[e + 1] = 0; KV(&n, "Board", v);
            Sys_ReadFanPct(&fan); e = XvU2A(fan, v); v[e] = '%'; v[e + 1] = 0; KV(&n, "Fan", v);
        }
        else { KV(&n, "CPU", "---"); KV(&n, "Board", "---"); KV(&n, "Fan", "---"); }
    } break;
    case LCD_PAGE_MEM: {
        char v[16]; int e, total = Sys_RamMB(), fr = Sys_RamFreeMB();
        lstrcpynA(s_pgTitle, "Memory", sizeof(s_pgTitle));
        e = XvU2A(total, v); v[e] = 'M'; v[e + 1] = 'B'; v[e + 2] = 0; KV(&n, "Total", v);
        e = XvU2A(fr, v); v[e] = 'M'; v[e + 1] = 'B'; v[e + 2] = 0; KV(&n, "Free", v);
        e = XvU2A(total - fr, v); v[e] = 'M'; v[e + 1] = 'B'; v[e + 2] = 0; KV(&n, "Used", v);
    } break;
    case LCD_PAGE_DISK: {
        static const char letters[4] = { 'C', 'E', 'F', 'G' }; int i;
        lstrcpynA(s_pgTitle, "Disks", sizeof(s_pgTitle));
        /* full-width rows ("C: 7.9G/8.0G"): the half-res value column is too
           narrow for free/total on large drives, so drop the split layout. */
        for (i = 0; i < 4; i++) {
            char root[4], val[28]; DWORD attr; ULARGE_INTEGER fc, total, fb; int p;
            root[0] = letters[i]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
            attr = GetFileAttributesA(root);
            if (attr == 0xFFFFFFFF || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
            val[0] = letters[i]; val[1] = ':'; val[2] = ' '; p = 3;
            if (!GetDiskFreeSpaceExA(root, &fc, &total, &fb)) {
                val[p++] = '-'; val[p++] = '-'; val[p] = 0; KV(&n, "", val); continue;
            }
            {
                DWORD fmb = (DWORD)(fb.QuadPart / (1024ULL * 1024ULL)); DWORD tmb = (DWORD)(total.QuadPart / (1024ULL * 1024ULL));
                p += XvSizeC(val + p, fmb); val[p++] = '/'; p += XvSizeC(val + p, tmb); val[p] = 0;
            }
            KV(&n, "", val);
        }
    } break;
    case LCD_PAGE_NET: {
        const char* ip = Net_Ip();
        lstrcpynA(s_pgTitle, "Network", sizeof(s_pgTitle));
        KV(&n, "Link", Net_LinkUp() ? "Up" : "Down");
        {
            char ipl[26]; lstrcpynA(ipl, "IP ", sizeof(ipl));
            lstrcatA(ipl, (ip && ip[0]) ? ip : "---"); KV(&n, "", ipl);
        }
    } break;
    case LCD_PAGE_FTP: {
        int st = Ftp_Status();
        lstrcpynA(s_pgTitle, "FTP", sizeof(s_pgTitle));
        if (st == 0) { KV(&n, "State", "Off"); }
        else if (st == 3) {
            unsigned long done = 0, total = 0; char v[12]; int e, pct = 0;
            KV(&n, "State", "Transfer"); Ftp_Progress(&done, &total);
            if (total > 0) {
                unsigned long d2 = done, t2 = total;
                while (t2 > 0x7FFFFUL) { t2 >>= 4; d2 >>= 4; }
                if (t2 > 0) pct = (int)(d2 * 100 / t2);
                e = XvU2A(pct, v); v[e] = '%'; v[e + 1] = 0; KV(&n, "Progress", v);
                s_pgHasBar = 1; s_pgBarDone = done; s_pgBarTotal = total;
            }
            else { KV(&n, "Progress", "..."); }
        }
        else { KV(&n, "State", st == 2 ? "Connected" : "Listening"); }
    } break;
    case LCD_PAGE_CLOCK: {
        SysClock c; char v[20]; int p;
        Sys_GetClock(&c); lstrcpynA(s_pgTitle, "Clock", sizeof(s_pgTitle));
        p = XvU2A(c.year, v);
        v[p++] = '-'; v[p++] = (char)('0' + c.mon / 10); v[p++] = (char)('0' + c.mon % 10);
        v[p++] = '-'; v[p++] = (char)('0' + c.day / 10); v[p++] = (char)('0' + c.day % 10); v[p] = 0; KV(&n, "Date", v);
        p = 0;
        v[p++] = (char)('0' + c.hour / 10); v[p++] = (char)('0' + c.hour % 10); v[p++] = ':';
        v[p++] = (char)('0' + c.min / 10);  v[p++] = (char)('0' + c.min % 10);  v[p++] = ':';
        v[p++] = (char)('0' + c.sec / 10);  v[p++] = (char)('0' + c.sec % 10);  v[p] = 0; KV(&n, "Time", v);
    } break;
    case LCD_PAGE_NOWPLAYING: {
        const char* name = ""; DWORD el = 0; char tm[8]; int secs, mm, ss;
        lstrcpynA(s_pgTitle, "Now Playing", sizeof(s_pgTitle));
        if (!Settings_ShuffleNowPlaying(&name, &el)) { KV(&n, "", "(stopped)"); }
        else {
            KV(&n, "", name);
            secs = (int)(el / 1000); mm = secs / 60; ss = secs % 60; if (mm > 99) { mm = 99; ss = 59; }
            tm[0] = (char)('0' + (mm / 10) % 10); tm[1] = (char)('0' + mm % 10); tm[2] = ':';
            tm[3] = (char)('0' + ss / 10);      tm[4] = (char)('0' + ss % 10); tm[5] = 0;
            KV(&n, "Elapsed", tm);
        }
    } break;
    default: lstrcpynA(s_pgTitle, "", sizeof(s_pgTitle)); break;
    }
    s_pgLineN = n;
}

/* Build the rotation from the LCD's enabled-pages mask so the user's existing
   page toggles carry straight over. Now Playing only joins while Shuffle is
   active. Falls back to all data pages if nothing is enabled. */
static int BuildOrder(int* order) {
    static const int bits[6] = { LCD_PAGE_TEMPS, LCD_PAGE_MEM, LCD_PAGE_DISK,
                                 LCD_PAGE_NET, LCD_PAGE_FTP, LCD_PAGE_CLOCK };
    int mask = s_cfg.pages;
    int n = 0, i;
    const char* nm; DWORD el;
    if (mask == 0) mask = LCD_PAGE_ALL;
    for (i = 0; i < 6; i++) if (mask & bits[i]) order[n++] = bits[i];
    if ((mask & LCD_PAGE_NOWPLAYING) && Settings_ShuffleNowPlaying(&nm, &el))
        order[n++] = LCD_PAGE_NOWPLAYING;
    if (n == 0) order[n++] = LCD_PAGE_CLOCK;
    return n;
}

/* integer nearest-neighbour letterbox of an RGBA source into a dim x dim RGB565
   square (black bars) */
static void ArtLetterbox(const unsigned char* src, int sw, int sh, uint16_t* out, int dim) {
    int i, x, y, dw, dh, ox, oy;
    for (i = 0; i < dim * dim; i++) out[i] = 0;
    if (!src || sw <= 0 || sh <= 0) return;
    if (sw >= sh) { dw = dim; dh = sh * dim / sw; }
    else { dh = dim; dw = sw * dim / sh; }
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    if (dw > dim) dw = dim; if (dh > dim) dh = dim;
    ox = (dim - dw) / 2; oy = (dim - dh) / 2;
    for (y = 0; y < dh; y++) {
        int sy = y * sh / dh; if (sy > sh - 1) sy = sh - 1;
        for (x = 0; x < dw; x++) {
            int sx = x * sw / dw; const unsigned char* px;
            if (sx > sw - 1) sx = sw - 1;
            px = src + (sy * sw + sx) * 4;
            out[(oy + y) * dim + (ox + x)] = XvCli_Rgb565(px[0], px[1], px[2]);
        }
    }
}

/* compose one data-page frame: plasma + translucent header/rows + glowing text */
static void ComposeFrame(int t) {
    uint16_t acc = XvCli_Rgb565(s_acc.r, s_acc.g, s_acc.b);
    uint16_t txt = XvCli_Rgb565(s_text.r, s_text.g, s_text.b);
    uint16_t dim = XvCli_Rgb565(s_dim.r, s_dim.g, s_dim.b);
    int i, y, hdr, lineH, top, valX, maxRows;
    RenderPlasma(t);
    hdr = (s_rh >= 90) ? 17 : 13;                /* header height (panel-aware) */
    lineH = (s_rh >= 90) ? 18 : 16;
    top = hdr + 5;
    valX = (s_rw * 9) / 20; if (valX < 36) valX = 36;
    DarkenBand(0, 0, s_rw, hdr, 96);               /* header band */
    FbRect(0, hdr, s_rw, 2, acc);                  /* accent underline */
    FbText(4, (s_rh >= 90) ? 1 : 0, 1, acc, 1, s_pgTitle);
    maxRows = (s_rh - top) / lineH; if (maxRows < 1) maxRows = 1;
    y = top;
    for (i = 0; i < s_pgLineN && i < maxRows; i++) {
        DarkenBand(4, y - 1, s_rw - 8, lineH - 2, 150);
        if (s_pgLines[i].label[0]) {
            FbText(4, y, 1, dim, 1, s_pgLines[i].label);
            FbText(valX, y, 1, txt, 1, s_pgLines[i].val);
        }
        else {
            FbText(4, y, 1, txt, 1, s_pgLines[i].val);
        }
        y += lineH;
    }
    if (s_pgHasBar && y + 8 <= s_rh) {
        unsigned long done = s_pgBarDone, total = s_pgBarTotal;
        int x0 = 4, w = s_rw - 8, fw = 0;
        while (total > 0x7FFFFUL) { total >>= 4; done >>= 4; }
        if (total > 0) fw = (int)((unsigned long)w * done / total);
        if (fw < 0) fw = 0; if (fw > w) fw = w;
        FbRect(x0, y + 1, w, 6, dim);
        FbRect(x0, y + 1, fw, 6, acc);
    }
}

/* animated brand splash: plasma behind a glowing DarkDash / Darkone83 */
/* lerp colour a->b (num/den) -> RGB565  (splash animation) */
static uint16_t MixRgb565(Rgb a, Rgb b, int num, int den) {
    int r, g, bl;
    if (den <= 0) den = 1;
    if (num < 0) num = 0; if (num > den) num = den;
    r = (a.r * (den - num) + b.r * num) / den;
    g = (a.g * (den - num) + b.g * num) / den;
    bl = (a.b * (den - num) + b.b * num) / den;
    return XvCli_Rgb565(r, g, bl);
}

/* additive brighten a region -- the specular shine sweep. add ~0..255 / channel */
static void BrightBand(int x0, int y0, int w, int h, int add) {
    int x, y, ar, ag, ab;
    ar = add * 31 / 255; ag = add * 63 / 255; ab = add * 31 / 255;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    for (y = y0; y < y0 + h && y < s_rh; y++) {
        uint16_t* dst = s_fb + y * s_rw;
        for (x = x0; x < x0 + w && x < s_rw; x++) {
            uint16_t c = dst[x];
            int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            r += ar; if (r > 31) r = 31;
            g += ag; if (g > 63) g = 63;
            b += ab; if (b > 31) b = 31;
            dst[x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

/* one glyph with a 1px drop shadow (legibility over plasma) */
static void FbGlyphSh(int x, int y, uint16_t col, int ch) {
    FbGlyph(x + 1, y + 1, 1, XvCli_Rgb565(0, 0, 0), ch);
    FbGlyph(x, y, 1, col, ch);
}

static void DrawSplash(void) {
    /* multi-phase opening: ignite -> letter cascade -> underline + subtitle
       -> specular shine. One BlitScaled+Present per frame; honours Stopping(). */
    static const char k_title[] = "DarkDash";   /* 8 glyphs, no descenders */
    static const char k_sub[] = "Darkone83";  /* 9 glyphs                */
    enum {
        IGNITE = 10, CAS_START = 6, STAG = 2, FLIGHT = 7, DROP = 18, SETTLE = 3,
        UND_START = 24, UND_DUR = 10, SUB_START = 26, SUB_DUR = 8,
        SHN_START = 32, SHN_END = 44, TOTAL = 47
    };
    int nT = 8, nS = 9;
    int tx0 = (s_rw - 8 * 8) / 2;
    int sx0 = (s_rw - 9 * 8) / 2;
    int ty = s_rh / 2 - 18;                      /* centre the brand block  */
    int sy, uy, cardY, cardH, uFull = 8 * 8;
    Rgb bright; uint16_t txt, acc; int F;

    if (ty < 2) ty = 2;
    sy = ty + 20; uy = ty + 17;
    cardY = ty - 4; cardH = (sy + 16) - cardY;
    if (s_palDirty) BuildPalette();
    bright.r = (s_acc.r + 255) / 2; bright.g = (s_acc.g + 255) / 2; bright.b = (s_acc.b + 255) / 2;
    txt = XvCli_Rgb565(s_text.r, s_text.g, s_text.b);
    acc = XvCli_Rgb565(s_acc.r, s_acc.g, s_acc.b);

    for (F = 0; F <= TOTAL; F++) {
        int i;
        if (Stopping()) return;

        RenderPlasma(F * 6);

        /* A: plasma rises out of black */
        if (F < IGNITE) DarkenBand(0, 0, s_rw, s_rh, F * 256 / IGNITE);

        /* contrast card behind the brand, eased in just before the letters */
        if (F >= CAS_START - 2) {
            int k = 150;
            if (F < CAS_START + 4)
                k = 256 - (256 - 150) * (F - (CAS_START - 2)) / 6;
            DarkenBand(0, cardY, s_rw, cardH, k);
        }

        /* B: title letters cascade down, flash bright on landing, settle to text */
        for (i = 0; i < nT; i++) {
            int st = CAS_START + i * STAG;
            int ls, rem, off; uint16_t col;
            if (F < st) continue;
            ls = F - st; if (ls > FLIGHT) ls = FLIGHT;
            rem = FLIGHT - ls;
            off = (DROP * rem * rem) / (FLIGHT * FLIGHT);    /* ease-out drop */
            if (ls < FLIGHT)
                col = XvCli_Rgb565(bright.r, bright.g, bright.b);
            else if (F - st < FLIGHT + SETTLE)
                col = MixRgb565(bright, s_text, F - st - FLIGHT, SETTLE);
            else
                col = txt;
            FbGlyphSh(tx0 + i * 8, ty - off, col, (unsigned char)k_title[i]);
        }

        /* C: accent underline draws outward from centre */
        if (F >= UND_START) {
            int us = F - UND_START, w;
            if (us > UND_DUR) us = UND_DUR;
            w = uFull * us / UND_DUR;
            FbRect(tx0 + (uFull - w) / 2, uy, w, 2, acc);
        }

        /* C: subtitle slides up + brightens in */
        if (F >= SUB_START) {
            int ss = F - SUB_START, p = ss, off, j; uint16_t col;
            if (p > SUB_DUR) p = SUB_DUR;
            off = 8 * (SUB_DUR - p) / SUB_DUR;               /* slide +8 -> 0 */
            col = MixRgb565(s_dim, s_text, p, SUB_DUR);      /* dim -> text   */
            for (j = 0; j < nS; j++)
                FbGlyphSh(sx0 + j * 8, sy + off, col, (unsigned char)k_sub[j]);
        }

        /* D: specular shine sweeps across the brand once */
        if (F >= SHN_START && F <= SHN_END) {
            int sh = F - SHN_START, span = SHN_END - SHN_START;
            int cx = ((s_rw + 16) * sh / span) - 8;
            BrightBand(cx - 3, cardY, 7, cardH, 80);
            BrightBand(cx - 1, cardY, 3, cardH, 70);
        }

        XvCli_BlitScaled(0, 0, s_rw, s_rh, s_scale, s_fb);
        XvCli_Present();
        Sleep(12);
    }
}

/* brief plasma fade-to-black on hand-off / disable */
static void DrawOutro(void) {
    int i;
    for (i = 0; i < 4; i++) {
        DarkenBand(0, 0, s_rw, s_rh, 150);
        XvCli_BlitScaled(0, 0, s_rw, s_rh, s_scale, s_fb);
        XvCli_Present();
        Sleep(40);
    }
    XvCli_Clear(0);
    XvCli_Present();
}

/* the persistent frame left on the panel while a game runs: plasma backdrop,
   cover art, and the title -- all composited, then one blit, no clear after */
static void DrawNowPlayingFrame(const char* title, const char* xbePath) {
    uint16_t acc = XvCli_Rgb565(s_acc.r, s_acc.g, s_acc.b);
    uint16_t txt = XvCli_Rgb565(s_text.r, s_text.g, s_text.b);
    int dim, ax, ay, hdr; unsigned char* rgba; int rw = 0, rh = 0;
    if (s_palDirty) BuildPalette();
    RenderPlasma(0);
    hdr = (s_rh >= 90) ? 17 : 13;
    DarkenBand(0, 0, s_rw, hdr, 96);
    FbRect(0, hdr, s_rw, 2, acc);
    FbText(4, (s_rh >= 90) ? 1 : 0, 1, acc, 1, "Now Playing");
    if (s_rh >= 64) {                                  /* room for cover art */
        dim = s_rh - hdr - 20;
        if (dim > s_rw - 16) dim = s_rw - 16;
        if (dim > XV_ART_DIM) dim = XV_ART_DIM;
        if (dim < 16) dim = 16;
        rgba = Launcher_LoadArtRGBA(xbePath, &rw, &rh);
        if (rgba && rw > 0 && rh > 0) {
            ArtLetterbox(rgba, rw, rh, s_art, dim);
            ax = (s_rw - dim) / 2; ay = hdr + 5;
            FbRect(ax - 1, ay - 1, dim + 2, dim + 2, acc);
            FbBlitImg(ax, ay, s_art, dim, dim);
            DD_StbFree(rgba);
        }
    }
    if (title && title[0]) {
        DarkenBand(0, s_rh - 15, s_rw, 15, 90);
        FbText(4, s_rh - 13, 1, txt, 1, title);
    }
    XvCli_BlitScaled(0, 0, s_rw, s_rh, s_scale, s_fb);
    XvCli_Present();
}

/* rotate the data pages; plasma animates every frame, content refreshes ~1/sec */
/* returns 0 if a stop was requested, 1 if the panel was unplugged */
static int RunPages(void) {
    int order[8]; int n, cur = 0, t = 0, fails = 0; DWORD pageStart, lastBuild, lastLink;
    if (s_palDirty) BuildPalette();
    n = BuildOrder(order);
    BuildContent(order[0]);
    pageStart = lastBuild = lastLink = GetTickCount();
    while (!Stopping()) {
        DWORD now = GetTickCount();
        DWORD interval = (DWORD)s_cfg.intervalMs;   /* re-read live each frame */
        int rc;
        if (interval < 1000)  interval = 4000;
        if (interval > 30000) interval = 30000;
        /* authoritative unplug check (hub PORT_CONNECTION + USBD remove latch),
           polled ~1/s -- does not depend on bulk-transfer timeouts. */
        if (now - lastLink >= 1000) {
            lastLink = now;
            if (!XvXbox_StillConnected()) return 1;
        }
        if (InterlockedExchange(&s_panelDirty, 0)) {  /* live panel switch */
            XvCli_SetPanel(s_cfg.panel);
            s_haveInfo = (XvCli_QueryInfo(&s_info) == 0);
            XvSetGeom(s_haveInfo ? s_info.width : 320, s_haveInfo ? s_info.height : 240);
            XvCli_Clear(XvCli_Rgb565(s_bg.r, s_bg.g, s_bg.b));
        }
        if (InterlockedExchange(&s_brightDirty, 0))  /* apply brightness live */
            XvCli_SetBrightness(s_cfg.brightness);
        if (s_palDirty) BuildPalette();
        if (now - pageStart >= interval) {
            n = BuildOrder(order);
            cur++; if (cur >= n) cur = 0;
            BuildContent(order[cur]); pageStart = now; lastBuild = now;
        }
        else if (now - lastBuild >= 1000) {
            BuildContent(order[cur]); lastBuild = now;
        }
        ComposeFrame(t);
        rc = XvCli_BlitScaled(0, 0, s_rw, s_rh, s_scale, s_fb);
        XvCli_Present();
        if (rc != 0) { if (++fails >= 4) return 1; }   /* transfers failing -> unplugged (fallback if USBD RemoveDevice never fires) */
        else fails = 0;
        t += 5;
        if (SleepChecked(15)) break;
    }
    return 0;
}

/* ---- the panel thread ------------------------------------------------- */

static DWORD WINAPI XvProc(LPVOID arg) {
    (void)arg;

    if (SleepChecked(XV_WARMUP_MS)) return 0;            /* USBD settle (s3.3) */

    /* Hotplug outer loop: keep (re)scanning for the panel until a real stop.
       The panel can be plugged in after boot, or pulled and replugged -- each
       fresh XvXbox_Init() re-walks the bus and reopens the pipes. */
    while (!Stopping()) {
        int rc;

        if (XvXbox_Init() != XV_XBOX_OK || !XvXbox_IsReady()) {
            XvXbox_Shutdown();                          /* free any partial bring-up */
            if (SleepChecked(XV_RESCAN_MS)) break;      /* poll for hotplug-in */
            continue;
        }

        /* connected -- bring the panel up */
        XvCli_Ping();
        XvCli_SetPanel(s_cfg.panel);                        /* apply saved preset  */
        InterlockedExchange(&s_panelDirty, 0);
        s_haveInfo = (XvCli_QueryInfo(&s_info) == 0);       /* geometry post-switch */
        XvSetGeom(s_haveInfo ? s_info.width : 320, s_haveInfo ? s_info.height : 240);
        XvCli_SetPresentMode(XV_PRESENT_MANUAL);
        XvCli_SetBrightness(s_cfg.brightness);
        InterlockedExchange(&s_brightDirty, 0);
        XvCli_Clear(XvCli_Rgb565(s_bg.r, s_bg.g, s_bg.b));   /* clear on init (s3.7) */
        XvCli_Present();

        DrawSplash();
        InterlockedExchange(&s_ready, 1);
        rc = RunPages();                                /* 0 = stop, 1 = unplugged */
        InterlockedExchange(&s_ready, 0);

        if (Stopping()) {                               /* real shutdown / hand-off */
            if (InterlockedCompareExchange(&s_npMode, 0, 0))
                DrawNowPlayingFrame(s_npTitle, s_npXbe);  /* freeze art, no clear */
            else
                DrawOutro();
            XvXbox_Shutdown();
            break;
        }

        /* rc == 1: panel was unplugged -- tear down and re-scan for hotplug */
        s_haveInfo = 0;
        XvXbox_Shutdown();
        if (SleepChecked(XV_RESCAN_MS)) break;
    }
    InterlockedExchange(&s_ready, 0);
    return 0;
}

/* ---- public ----------------------------------------------------------- */

int  XView_IsEnabled(void) { if (!s_cfgLoaded) CfgLoad(); return s_cfg.enable; }
int  XView_Brightness(void) { if (!s_cfgLoaded) CfgLoad(); return s_cfg.brightness; }
int  XView_IsReady(void) { return InterlockedCompareExchange(&s_ready, 0, 0) != 0; }

void XView_Start(void) {
    if (!s_cfgLoaded) CfgLoad();
    if (!s_cfg.enable) return;
    if (s_thread) return;
    InterlockedExchange(&s_stop, 0);
    InterlockedExchange(&s_ready, 0);
    s_thread = CreateThread(NULL, 0, XvProc, NULL, 0, NULL);
    if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
}

void XView_Stop(void) {
    if (!s_thread) return;
    InterlockedExchange(&s_stop, 1);
    WaitForSingleObject(s_thread, XV_STOP_WAIT_MS);
    CloseHandle(s_thread);
    s_thread = NULL;
    InterlockedExchange(&s_ready, 0);
}

void XView_NowPlayingLaunch(const char* title, const char* xbePath) {
    /* Hand the panel thread a title + cover art, ask it to freeze that frame
       (instead of the black outro), then shut the USB down -- the last frame
       stays on the panel while the game runs, mirroring LCD Now Playing. */
    if (!s_thread) return;
    lstrcpynA(s_npTitle, title ? title : "", (int)sizeof(s_npTitle));
    lstrcpynA(s_npXbe, xbePath ? xbePath : "", (int)sizeof(s_npXbe));
    InterlockedExchange(&s_npMode, 1);
    InterlockedExchange(&s_stop, 1);
    WaitForSingleObject(s_thread, XV_NP_WAIT_MS);
    CloseHandle(s_thread);
    s_thread = NULL;
    InterlockedExchange(&s_ready, 0);
    InterlockedExchange(&s_npMode, 0);
}

void XView_SetEnabled(int on) {
    if (!s_cfgLoaded) CfgLoad();
    on = on ? 1 : 0;
    if (s_cfg.enable == on) return;
    s_cfg.enable = on;
    CfgSave();
    if (on) XView_Start();
    else    XView_Stop();
}

void XView_SetBrightness(int duty0to255) {
    if (!s_cfgLoaded) CfgLoad();
    if (duty0to255 < 0)   duty0to255 = 0;
    if (duty0to255 > 255) duty0to255 = 255;
    s_cfg.brightness = duty0to255;
    CfgSave();
    InterlockedExchange(&s_brightDirty, 1);   /* push to the live panel now */
}

int  XView_Pages(void) { if (!s_cfgLoaded) CfgLoad(); return s_cfg.pages; }
void XView_SetPages(int mask) { if (!s_cfgLoaded) CfgLoad(); s_cfg.pages = mask; CfgSave(); }
void XView_TogglePage(int bit) { if (!s_cfgLoaded) CfgLoad(); s_cfg.pages ^= bit; CfgSave(); }
int  XView_IntervalMs(void) { if (!s_cfgLoaded) CfgLoad(); return s_cfg.intervalMs; }
int  XView_Panel(void) { if (!s_cfgLoaded) CfgLoad(); return s_cfg.panel; }
void XView_SetPanel(int panel) {
    if (!s_cfgLoaded) CfgLoad();
    s_cfg.panel = (panel == XV_PANEL_A) ? XV_PANEL_A : XV_PANEL_B;
    CfgSave();
    InterlockedExchange(&s_panelDirty, 1);   /* live re-init on the panel thread */
}
void XView_SetIntervalMs(int ms) {
    if (!s_cfgLoaded) CfgLoad();
    if (ms < 1000)  ms = 1000;
    if (ms > 30000) ms = 30000;
    s_cfg.intervalMs = ms; CfgSave();
}