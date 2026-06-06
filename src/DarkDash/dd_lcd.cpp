/*---------------------------------------------------------------------------
    dd_lcd.cpp -- see dd_lcd.h.

    US2066 20x4 character-OLED driver over SMBus, retooled from XbDiag for
    DarkDash. Self-contained SMBus helpers (HalReadSMBusValue/HalWriteSMBusValue),
    a shadow buffer so unchanged cells skip the (slow) bus, configurable address
    (0x3C/0x3D), and data-driven rotating pages fed from DarkDash's public
    Sys, Net and Ftp APIs. C89 style, file-scope statics. No CerBIOS gating.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"
#include "dd_lcd.h"
#include "dd_sysinfo.h"
#include "dd_net.h"
#include "dd_ftp.h"
#include "dd_version.h"

#define LCD_COLS   20
#define LCD_ROWS   4

/* 8-bit (software-shifted) SMBus addresses for the two 7-bit choices */
#define LCD_ADDR8_3C  0x78
#define LCD_ADDR8_3D  0x7A

#define HD44780_CLEAR     0x01
#define HD44780_HOME      0x02
#define HD44780_ENTRY_SET 0x06
#define HD44780_DISP_ON   0x0C
#define HD44780_FUNC_SET  0x38
#define HD44780_DDRAM(a)  (0x80 | (BYTE)(a))

static const BYTE k_rowBase[4] = { 0x00, 0x20, 0x40, 0x60 };
#define VAL_COL 9

#define DDLCD_MAGIC   0x444C4344UL   /* 'DLCD' */
#define DDLCD_VER     2

/* ---- persisted config --------------------------------------------------- */
typedef struct {
    DWORD magic;
    DWORD version;
    int   enabled;
    int   addrChoice;     /* LCD_ADDR_3C / LCD_ADDR_3D */
    int   pages;          /* page-type bitmask         */
    int   intervalMs;     /* rotation interval         */
    int   brightness;     /* v2: OLED contrast 0..255  */
} LcdBlob;

static LcdBlob s_cfg;
static int     s_cfgLoaded = 0;

/* ---- runtime state ------------------------------------------------------ */
static int   s_present = 0;
static BYTE  s_addr8 = LCD_ADDR8_3C;     /* active 8-bit address           */
static int   s_pageOrder[8];               /* enabled page bits, in order    */
static int   s_pageN = 0;                  /* count of enabled pages         */
static int   s_pageCur = 0;                /* index into s_pageOrder         */
static DWORD s_pageTimer = 0;
static DWORD s_sensorTimer = 0;

/* shadow buffer: skip SMBus writes for unchanged cells */
static BYTE s_shadow[LCD_ROWS][LCD_COLS];
static int  s_hwRow = -1, s_hwCol = -1;
static int  s_lRow = -1, s_lCol = -1;

/* which custom glyph set is currently in CGRAM (0=none, 1=bar, 2=splash).
   Reloading is only done when the needed set isn't already there, so the FTP
   bar's per-tick diff updates aren't disturbed. */
#define GLYPHS_BAR     1
#define GLYPHS_SPLASH  2
static int  s_glyphSet = 0;

/* cached sensor reads (refreshed ~1/sec, not every frame) */
static int  s_cpuC = 0, s_boardC = 0, s_fanPct = 0;
static int  s_sensorOK = 0;

/* ---- tiny str helpers (no CRT) ------------------------------------------ */
static int  LL(const char* s) { int n = 0; while (s && s[n])n++; return n; }
static void LCopy(char* d, int cap, const char* s) { int i = 0; if (cap <= 0)return; while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static void IToA(int v, char* out) { char t[12]; int n = 0, i = 0, neg = 0; if (v < 0) { neg = 1; v = -v; } if (v == 0)t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; } if (neg)out[i++] = '-'; while (n)out[i++] = t[--n]; out[i] = 0; }

/* ---- SMBus low level ---------------------------------------------------- */
static int SmbW(BYTE reg, BYTE val) {
    return HalWriteSMBusValue(s_addr8, reg, FALSE, (DWORD)val) == 0;
}
static int SmbProbe(void) {
    DWORD v = 0;
    return HalReadSMBusValue(s_addr8, 0x00, FALSE, &v) == 0;
}

/* ---- LCD primitives ----------------------------------------------------- */
static void LCDCmd(BYTE cmd) {
    SmbW(0x80, cmd);
    s_hwRow = -1; s_hwCol = -1; s_lRow = -1; s_lCol = -1;
}

static void LCDChar(BYTE data) {
    if (data != 0xFF && (data < 0x20 || data > 0x7E)) data = ' ';
    if (s_lRow < 0 || s_lRow >= LCD_ROWS || s_lCol < 0 || s_lCol >= LCD_COLS) {
        SmbW(0x40, data); return;
    }
    if (s_shadow[s_lRow][s_lCol] == data) {
        s_lCol++; if (s_lCol >= LCD_COLS) { s_lRow++; s_lCol = 0; }
        return;
    }
    if (s_hwRow != s_lRow || s_hwCol != s_lCol) {
        SmbW(0x80, HD44780_DDRAM(k_rowBase[s_lRow] + (BYTE)s_lCol));
        s_hwRow = s_lRow; s_hwCol = s_lCol;
    }
    SmbW(0x40, data);
    s_shadow[s_lRow][s_lCol] = data;
    s_lCol++; s_hwCol++;
    if (s_lCol >= LCD_COLS) { s_lRow++; s_lCol = 0; s_hwRow++; s_hwCol = 0; }
}

static void LCDGoto(int row, int col) {
    if (row < 0 || row >= LCD_ROWS || col < 0 || col >= LCD_COLS) return;
    SmbW(0x80, HD44780_DDRAM(k_rowBase[row] + (BYTE)col));
    s_hwRow = row; s_hwCol = col; s_lRow = row; s_lCol = col;
}

static void LCDPuts(const char* s, int width) {
    int n = 0;
    while (s && *s && n < width) { BYTE c = (BYTE)*s++; if (c < 0x20 || c>0x7E)c = ' '; LCDChar(c); n++; }
    while (n < width) { LCDChar(' '); n++; }
}

static void LCDHeader(const char* title) {
    char buf[21]; int pos = 0, i, len = LL(title), total, lDash, rDash;
    LCDGoto(0, 0);
    total = LCD_COLS - len - 2; if (total < 0) total = 0;
    lDash = total / 2; rDash = total - lDash;
    for (i = 0; i < lDash && pos < LCD_COLS; i++) buf[pos++] = '-';
    if (pos < LCD_COLS) buf[pos++] = ' ';
    for (i = 0; i < len && pos < LCD_COLS; i++) buf[pos++] = title[i];
    if (pos < LCD_COLS) buf[pos++] = ' ';
    for (i = 0; i < rDash && pos < LCD_COLS; i++) buf[pos++] = '-';
    buf[pos] = 0;
    LCDPuts(buf, LCD_COLS);
}

static void LCDLabelVal(int row, const char* label, const char* val) {
    int n = 0;
    LCDGoto(row, 0);
    while (label && *label && n < VAL_COL - 1) { LCDChar((BYTE)*label++); n++; }
    while (n < VAL_COL) { LCDChar(' '); n++; }
    LCDPuts(val, LCD_COLS - VAL_COL);
}

static void LCDCenter(char* out, const char* s) {
    int len = LL(s), total, lPad, rPad, pos = 0, i;
    if (len >= LCD_COLS) { for (i = 0; i < LCD_COLS; i++)out[i] = s[i]; out[LCD_COLS] = 0; return; }
    total = LCD_COLS - len; lPad = total / 2; rPad = total - lPad;
    for (i = 0; i < lPad; i++)out[pos++] = ' ';
    for (i = 0; i < len; i++)out[pos++] = s[i];
    for (i = 0; i < rPad; i++)out[pos++] = ' ';
    out[pos] = 0;
}

/* center 's' within an arbitrary field 'width' (<= 20) */
static void LCDCenterW(char* out, const char* s, int width) {
    int len = LL(s), total, lPad, rPad, pos = 0, i;
    if (width > LCD_COLS) width = LCD_COLS;
    if (len >= width) { for (i = 0; i < width; i++)out[i] = s[i]; out[width] = 0; return; }
    total = width - len; lPad = total / 2; rPad = total - lPad;
    for (i = 0; i < lPad; i++)out[pos++] = ' ';
    for (i = 0; i < len; i++)out[pos++] = s[i];
    for (i = 0; i < rPad; i++)out[pos++] = ' ';
    out[pos] = 0;
}

static void ShadowInvalidate(void) {
    int r, c;
    for (r = 0; r < LCD_ROWS; r++) for (c = 0; c < LCD_COLS; c++) s_shadow[r][c] = 0x01;
    s_hwRow = -1; s_hwCol = -1; s_lRow = -1; s_lCol = -1;
}

static void LoadBarGlyphs(void);   /* forward: defined just below, used in init */
static void EnsureBarGlyphs(void);
static void EnsureSplashGlyphs(void);
static int  DiskPageCount(void);   /* forward: used by RebuildPageOrder */

/* Set OLED contrast (brightness), 0..255. The contrast command lives in the
   locked OLED command set, so we unlock it, set it, then relock and return to
   the fundamental command set. The function-set bytes mirror the working init
   (0x38) with only the RE bit flipped, so the display's line config is kept. */
static void LcdSetContrastHW(BYTE val) {
    LCDCmd(0x3A);   /* function set, RE=1 (extended)   -- 0x38 | RE             */
    LCDCmd(0x79);   /* OLED Characterization, SD=1     -- OLED commands enabled */
    LCDCmd(0x81);   /* Set Contrast Control                                     */
    LCDCmd(val);    /* contrast value (second byte of the command)             */
    LCDCmd(0x78);   /* OLED Characterization, SD=0     -- OLED commands locked  */
    LCDCmd(0x38);   /* function set, RE=0              -- back to fundamental   */
}

/* Smooth power-on fade: ramp contrast 0 -> target. Pure visual flourish using
   the same contrast command. Finer steps over ~1s read as a graceful fade-up
   rather than a snap; the US2066's low contrast range is shallow, so the extra
   steps mostly buy smoothness near the top end where the change is visible. */
static void LcdFadeTo(BYTE target) {
    int steps = 48, i, v;
    for (i = 1; i <= steps; i++) {
        v = (int)target * i / steps;
        LcdSetContrastHW((BYTE)v);
        Sleep(20);                  /* 48 * 20ms ~= 0.96s total */
    }
}

static void LCDHardwareInit(void) {
    Sleep(15);
    LCDCmd(HD44780_FUNC_SET); Sleep(5);
    LCDCmd(HD44780_FUNC_SET); Sleep(1);
    LCDCmd(HD44780_FUNC_SET);
    LCDCmd(HD44780_DISP_ON);
    LCDCmd(HD44780_CLEAR);    Sleep(2);
    LCDCmd(HD44780_ENTRY_SET);
    s_glyphSet = 0;                 /* force a fresh glyph load after a HW reset */
    EnsureBarGlyphs();             /* default runtime set (FTP bar)             */
    LcdSetContrastHW((BYTE)s_cfg.brightness);   /* apply saved brightness */
}

/* Custom CGRAM glyphs for a smooth progress bar. The US2066 has 8 user glyphs
   (codes 0..7) of 5x8 dots. We define 5 "partial fill" glyphs (codes 1..5) that
   fill 1..5 columns of the cell from the left, so a bar built from these has
   5 sub-steps per character -- a 16-cell bar gives 80 steps of resolution.
   Code 0 is left as the panel's space so plain text is unaffected. */
#define BAR_GLYPH_BASE  1            /* glyph codes 1..5 = 1..5 columns filled  */
static void LoadBarGlyphs(void) {
    int g, row;
    for (g = 1; g <= 5; g++) {
        /* CGRAM address for glyph g = g*8 (8 rows per glyph) */
        LCDCmd((BYTE)(0x40 | (g * 8)));
        for (row = 0; row < 8; row++) {
            /* fill the leftmost g columns: bit4=col0 ... bit0=col4.
               A run of g columns from the left = top (5-g) bits clear. */
            BYTE bits = (BYTE)(0x1F & ~((1 << (5 - g)) - 1));
            SmbW(0x40, bits);        /* write CGRAM data row                    */
        }
    }
    /* leave AC back on DDRAM so subsequent text writes land correctly */
    LCDCmd(HD44780_DDRAM(0));
}

/* Splash glyph set: box-drawing pieces for a framed boot screen. The splash and
   the FTP bar never share the screen (splash is init-only, the bar is runtime
   FTP-only), so the splash gets the full glyph budget by swapping the set in at
   boot and swapping the bar set back for normal operation.

   Codes: 1=TL 2=TR 3=BL 4=BR 5=Htop 6=Hbot 7=Vert. Each is 5x8 (bit4=leftmost
   column). The frame line sits high in the top row's cells and low in the bottom
   row's cells so it hugs the display edges. */
static const BYTE k_splashGlyphs[7][8] = {
    /* 1 TL */ { 0x00,0x07,0x04,0x04,0x04,0x04,0x04,0x04 },
    /* 2 TR */ { 0x00,0x1C,0x04,0x04,0x04,0x04,0x04,0x04 },
    /* 3 BL */ { 0x04,0x04,0x04,0x04,0x04,0x04,0x07,0x00 },
    /* 4 BR */ { 0x04,0x04,0x04,0x04,0x04,0x04,0x1C,0x00 },
    /* 5 H- top */ { 0x00,0x1F,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 6 H- bot */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x00 },
    /* 7 Vert */ { 0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04 }
};
static void LoadSplashGlyphs(void) {
    int g, row;
    for (g = 1; g <= 7; g++) {
        LCDCmd((BYTE)(0x40 | (g * 8)));
        for (row = 0; row < 8; row++) SmbW(0x40, k_splashGlyphs[g - 1][row]);
    }
    LCDCmd(HD44780_DDRAM(0));
}

/* Load only the glyph set that's needed; reloading swaps the CGRAM contents and
   invalidates the shadow so the screen redraws. */
static void EnsureBarGlyphs(void) {
    if (s_glyphSet == GLYPHS_BAR) return;
    LoadBarGlyphs();
    s_glyphSet = GLYPHS_BAR;
    ShadowInvalidate();          /* glyph appearance changed -> force full redraw */
}
static void EnsureSplashGlyphs(void) {
    if (s_glyphSet == GLYPHS_SPLASH) return;
    LoadSplashGlyphs();
    s_glyphSet = GLYPHS_SPLASH;
    ShadowInvalidate();
}

/* write a raw glyph code (0..7) at the current cursor, bypassing the ASCII
   filter in LCDChar. Updates the shadow so diffing still works. */
static void LCDRawGlyph(BYTE code) {
    if (s_lRow < 0 || s_lRow >= LCD_ROWS || s_lCol < 0 || s_lCol >= LCD_COLS) {
        SmbW(0x40, code); return;
    }
    if (s_shadow[s_lRow][s_lCol] == code) {
        s_lCol++; if (s_lCol >= LCD_COLS) { s_lRow++; s_lCol = 0; }
        return;
    }
    if (s_hwRow != s_lRow || s_hwCol != s_lCol) {
        SmbW(0x80, HD44780_DDRAM(k_rowBase[s_lRow] + (BYTE)s_lCol));
        s_hwRow = s_lRow; s_hwCol = s_lCol;
    }
    SmbW(0x40, code);
    s_shadow[s_lRow][s_lCol] = code;
    s_lCol++; s_hwCol++;
    if (s_lCol >= LCD_COLS) { s_lRow++; s_lCol = 0; s_hwRow++; s_hwCol = 0; }
}

/* draw a progress bar of 'cells' columns at (row,col0) for fraction done/total.
   Uses the CGRAM partial-fill glyphs for sub-cell resolution. */
static void LCDBar(int row, int col0, int cells, unsigned long done, unsigned long total) {
    int subTotal, filledSub, i, full, rem;
    if (cells <= 0) return;
    subTotal = cells * 5;                       /* 5 sub-steps per cell          */
    if (total == 0) {
        filledSub = 0;
    }
    else {
        /* scale done/total to subTotal without overflow: shift both down until
           done fits in range where done*subTotal stays within 32 bits. */
        unsigned long d = done, t = total;
        while (t > 4000000UL) { d >>= 1; t >>= 1; }   /* keep d*subTotal bounded */
        if (t == 0) t = 1;
        filledSub = (int)((d * (unsigned long)subTotal) / t);
        if (filledSub > subTotal) filledSub = subTotal;
        if (filledSub < 0) filledSub = 0;
    }
    full = filledSub / 5;                        /* fully-filled cells           */
    rem = filledSub % 5;                        /* partial columns in next cell */
    LCDGoto(row, col0);
    for (i = 0; i < cells; i++) {
        if (i < full)              LCDRawGlyph(5);            /* full cell        */
        else if (i == full && rem) LCDRawGlyph((BYTE)rem);   /* partial cell     */
        else                       LCDChar(' ');             /* empty            */
    }
}

/* overflow-safe done/total * 100. A bare (done * 100) overflows 32 bits once
   done exceeds ~42MB, which made the percentage wrap to nonsense on large
   transfers even though the bar (which already scales down) stayed correct.
   Same shift-down trick as LCDBar so it never overflows. */
static int LcdPercent(unsigned long done, unsigned long total) {
    unsigned long d = done, t = total;
    if (t == 0) return 0;
    while (t > 40000000UL) { d >>= 1; t >>= 1; }   /* keep d*100 within 32 bits */
    if (t == 0) t = 1;
    if (d > t) d = t;
    return (int)((d * 100UL) / t);
}

/* ---- config persistence (lcd.dat, versioned + safe) --------------------- */
static void CfgReset(void) {
    s_cfg.magic = DDLCD_MAGIC;
    s_cfg.version = DDLCD_VER;
    s_cfg.enabled = 0;            /* off by default -- user opts in, avoids SMBus clashes */
    s_cfg.addrChoice = LCD_ADDR_3C;
    s_cfg.pages = LCD_PAGE_TEMPS | LCD_PAGE_NET | LCD_PAGE_FTP | LCD_PAGE_CLOCK;
    s_cfg.intervalMs = 5000;
    s_cfg.brightness = 200;       /* bright but not maxed (~78%) */
}

static void CfgLoad(void) {
    HANDLE h; DWORD got = 0; LcdBlob tmp;
    CfgReset();                   /* pre-seed current-version defaults */
    s_cfgLoaded = 1;
    h = CreateFileA("D:\\data\\lcd.dat", GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    ZeroMemory(&tmp, sizeof(tmp));
    if (ReadFile(h, &tmp, sizeof(tmp), &got, NULL) &&
        got >= (DWORD)(sizeof(DWORD) * 2) &&
        tmp.magic == DDLCD_MAGIC && tmp.version >= 1 && tmp.version <= DDLCD_VER) {
        /* copy only the bytes present on disk over the defaults, so an older
           (shorter) file keeps new fields at their defaults -- safe upgrade. */
        BYTE* dst = (BYTE*)&s_cfg; BYTE* src = (BYTE*)&tmp; DWORD i;
        for (i = 0; i < got && i < (DWORD)sizeof(s_cfg); i++) dst[i] = src[i];
        /* clamp / sanitize */
        if (s_cfg.addrChoice != LCD_ADDR_3C && s_cfg.addrChoice != LCD_ADDR_3D) s_cfg.addrChoice = LCD_ADDR_3C;
        s_cfg.pages &= LCD_PAGE_ALL;
        if (s_cfg.intervalMs < 1000) s_cfg.intervalMs = 1000;
        if (s_cfg.intervalMs > 30000) s_cfg.intervalMs = 30000;
        s_cfg.enabled = s_cfg.enabled ? 1 : 0;
        if (s_cfg.brightness < 16) s_cfg.brightness = 16;     /* never fully dark */
        if (s_cfg.brightness > 255) s_cfg.brightness = 255;
        s_cfg.version = DDLCD_VER;
        s_cfg.magic = DDLCD_MAGIC;
    }
    CloseHandle(h);
}

static void CfgSave(void) {
    HANDLE h; DWORD wr = 0;
    if (!s_cfgLoaded) { CfgReset(); s_cfgLoaded = 1; }
    CreateDirectoryA("D:\\data", NULL);
    h = CreateFileA("D:\\data\\lcd.dat", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, &s_cfg, sizeof(s_cfg), &wr, NULL);
    CloseHandle(h);
}

/* ---- build the rotation order from the page bitmask --------------------- */
static void RebuildPageOrder(void) {
    static const int k_bits[6] = {
        LCD_PAGE_TEMPS, LCD_PAGE_MEM, LCD_PAGE_DISK,
        LCD_PAGE_NET, LCD_PAGE_FTP, LCD_PAGE_CLOCK
    };
    int i;
    s_pageN = 0;
    for (i = 0; i < 6; i++) {
        if (s_cfg.pages & k_bits[i]) {
            s_pageOrder[s_pageN++] = k_bits[i];
            /* the disk view spans more than one rotating page when there are
               more user partitions than fit on a single screen (header + 3
               rows). Append the overflow page(s) right after the primary one so
               e.g. G: gets its own line instead of falling off the bottom. */
            if (k_bits[i] == LCD_PAGE_DISK && DiskPageCount() > 1)
                s_pageOrder[s_pageN++] = LCD_PAGE_DISK2;
        }
    }
    if (s_pageCur >= s_pageN) s_pageCur = 0;
}

/* ---- page renderers ----------------------------------------------------- */
static void PageSplash(void) {
    char l1[21], l2[21];
    int i;
    EnsureSplashGlyphs();          /* box-drawing glyph set */

    /* row 0: top border  TL + Htop x18 + TR */
    LCDGoto(0, 0);
    LCDRawGlyph(1);
    for (i = 0; i < LCD_COLS - 2; i++) LCDRawGlyph(5);
    LCDRawGlyph(2);

    /* rows 1-2: side borders with centered text in the 18-col interior */
    {
        char ver[21]; ver[0] = 'v'; ver[1] = ' '; ver[2] = 0;
        { int n = 0; while (DARKDASH_VERSION[n] && n < 18 - 2) { ver[2 + n] = DARKDASH_VERSION[n]; n++; } ver[2 + n] = 0; }
        LCDCenterW(l1, "DarkDash", LCD_COLS - 2);
        LCDCenterW(l2, ver, LCD_COLS - 2);
    }
    LCDGoto(1, 0); LCDRawGlyph(7); LCDPuts(l1, LCD_COLS - 2); LCDRawGlyph(7);
    LCDGoto(2, 0); LCDRawGlyph(7); LCDPuts(l2, LCD_COLS - 2); LCDRawGlyph(7);

    /* row 3: bottom border  BL + Hbot x18 + BR */
    LCDGoto(3, 0);
    LCDRawGlyph(3);
    for (i = 0; i < LCD_COLS - 2; i++) LCDRawGlyph(6);
    LCDRawGlyph(4);
}

static void PageTemps(void) {
    char v[16], n[12];
    LCDHeader("Temps");
    if (s_sensorOK) {
        IToA(s_cpuC, n); LCopy(v, sizeof(v), n); { int e = LL(v); v[e] = 'C'; v[e + 1] = 0; }
        LCDLabelVal(1, "CPU", v);
        IToA(s_boardC, n); LCopy(v, sizeof(v), n); { int e = LL(v); v[e] = 'C'; v[e + 1] = 0; }
        LCDLabelVal(2, "Board", v);
        IToA(s_fanPct, n); LCopy(v, sizeof(v), n); { int e = LL(v); v[e] = '%'; v[e + 1] = 0; }
        LCDLabelVal(3, "Fan", v);
    }
    else {
        LCDLabelVal(1, "CPU", "---");
        LCDLabelVal(2, "Board", "---");
        LCDLabelVal(3, "Fan", "---");
    }
}

static void PageMem(void) {
    char v[20], n[12]; int total = Sys_RamMB(), freeM = Sys_RamFreeMB();
    LCDHeader("Memory");
    IToA(total, n); LCopy(v, sizeof(v), n); { int e = LL(v); v[e] = 'M'; v[e + 1] = 'B'; v[e + 2] = 0; }
    LCDLabelVal(1, "Total", v);
    IToA(freeM, n); LCopy(v, sizeof(v), n); { int e = LL(v); v[e] = 'M'; v[e + 1] = 'B'; v[e + 2] = 0; }
    LCDLabelVal(2, "Free", v);
    IToA(total - freeM, n); LCopy(v, sizeof(v), n); { int e = LL(v); v[e] = 'M'; v[e + 1] = 'B'; v[e + 2] = 0; }
    LCDLabelVal(3, "Used", v);
}

/* compact size like "4.2G" or "512M" into out; returns chars written. */
static int LcdSizeC(char* out, DWORD mb) {
    char num[12]; int p = 0, k;
    if (mb < 1024) {
        IToA((int)mb, num);
        for (k = 0; num[k]; k++) out[p++] = num[k];
        out[p++] = 'M';
    }
    else {
        DWORD gb = mb / 1024, dec = (mb % 1024) * 10 / 1024;
        IToA((int)gb, num);
        for (k = 0; num[k]; k++) out[p++] = num[k];
        out[p++] = '.';
        out[p++] = (char)('0' + (int)dec);
        out[p++] = 'G';
    }
    return p;
}

/* "C: 4.2/8.0G" for a drive root; "C: --" if the drive isn't mounted. 20-col safe. */
static void LcdDiskLine(char letter, char* out) {
    char root[4]; ULARGE_INTEGER fc, total, fb; int p = 0;
    root[0] = letter; root[1] = ':'; root[2] = '\\'; root[3] = 0;
    out[p++] = letter; out[p++] = ':'; out[p++] = ' ';
    if (!GetDiskFreeSpaceExA(root, &fc, &total, &fb)) {
        out[p++] = '-'; out[p++] = '-'; out[p] = 0; return;
    }
    {
        DWORD freeMB = (DWORD)(fb.QuadPart / (1024ULL * 1024ULL));
        DWORD totMB = (DWORD)(total.QuadPart / (1024ULL * 1024ULL));
        p += LcdSizeC(out + p, freeMB);
        out[p++] = '/';
        p += LcdSizeC(out + p, totMB);
        out[p] = 0;
    }
}

/* Collect the present user partitions (C/E/F/G) into out[] (caller provides >=4).
   X/Y/Z (cache) are not listed here; they remain in the file manager/FTP/About.
   Returns the count. */
static int DiskPresentDrives(char* out) {
    static const char letters[4] = { 'C', 'E', 'F', 'G' };
    int i, n = 0;
    for (i = 0; i < 4; i++) {
        char root[4];
        DWORD attr;
        root[0] = letters[i]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
        attr = GetFileAttributesA(root);
        if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY)) out[n++] = letters[i];
    }
    return n;
}

/* number of rotating disk pages needed: header + 3 drive rows per page, so 3
   present drives per page. Always at least 1 so the page still appears (showing
   "no drives" effectively) even on the odd console where C: probes absent. */
#define LCD_DISK_PER_PAGE 3
static int DiskPageCount(void) {
    char present[4];
    int n = DiskPresentDrives(present);
    int pages = (n + LCD_DISK_PER_PAGE - 1) / LCD_DISK_PER_PAGE;   /* ceil */
    return pages < 1 ? 1 : pages;
}

/* Draw disk sub-page 'sub' (0-based): header + up to 3 present user partitions.
   Splitting across rotating pages means G: (the 4th partition) gets its own line
   on page 2 instead of being pushed off the bottom of a single 4-row screen. */
static void PageDisk(int sub) {
    char present[4];
    char line[24];
    int  n = DiskPresentDrives(present);
    int  start = sub * LCD_DISK_PER_PAGE;
    int  r;
    LCDHeader("Disks");
    for (r = 1; r <= 3; r++) {
        int idx = start + (r - 1);
        if (idx < n) {
            LcdDiskLine(present[idx], line);
            LCDGoto(r, 0); LCDPuts(line, LCD_COLS);
        }
        else {
            LCDGoto(r, 0); LCDPuts("", LCD_COLS);
        }
    }
}

static void PageNet(void) {
    const char* ip = Net_Ip();
    char buf[24]; int n = 0; const char* p;
    LCDHeader("Network");
    LCDLabelVal(1, "Link", Net_LinkUp() ? "Up" : "Down");
    /* IP on its own row with a short "IP " prefix so the full address fits. The
       label/value split (LCDLabelVal) only leaves 11 columns for the value,
       which clipped a 15-char dotted quad ("255.255.255.255"); a dedicated line
       gives the address the full width. */
    buf[n++] = 'I'; buf[n++] = 'P'; buf[n++] = ' ';
    p = (ip && ip[0]) ? ip : "---";
    while (*p && n < (int)sizeof(buf) - 1) buf[n++] = *p++;
    buf[n] = 0;
    LCDGoto(2, 0); LCDPuts(buf, LCD_COLS);
    LCDGoto(3, 0); LCDPuts("", LCD_COLS);
}

static void PageFtp(void) {
    int st = Ftp_Status();
    LCDHeader("FTP");
    if (st == 0) {
        LCDLabelVal(1, "State", "Off");
        LCDGoto(2, 0); LCDPuts("", LCD_COLS);
        LCDGoto(3, 0); LCDPuts("", LCD_COLS);
    }
    else if (st == 3) {
        unsigned long done = 0, total = 0; char v[20], n[12];
        LCDLabelVal(1, "State", "Transfer");
        Ftp_Progress(&done, &total);
        if (total > 0) {
            int pct = LcdPercent(done, total);
            IToA(pct, n); LCopy(v, sizeof(v), n); { int e = LL(v); v[e] = '%'; v[e + 1] = 0; }
            LCDLabelVal(2, "Progress", v);
            /* line 3: a smooth CGRAM bar spanning the full width */
            EnsureBarGlyphs();
            LCDBar(3, 0, LCD_COLS, done, total);
        }
        else {
            LCDLabelVal(2, "Progress", "...");
            LCDGoto(3, 0); LCDPuts("", LCD_COLS);
        }
    }
    else {
        LCDLabelVal(1, "State", st == 2 ? "Connected" : "Listening");
        LCDGoto(2, 0); LCDPuts("", LCD_COLS);
        LCDGoto(3, 0); LCDPuts("", LCD_COLS);
    }
}

/* shown briefly after a transfer ends so quick (small-file) transfers, which
   otherwise flash past in a single tick, leave something readable on the panel:
   a full bar at 100%. Lcd_Tick drives the linger timing. */
static void PageFtpComplete(void) {
    LCDHeader("FTP");
    LCDLabelVal(1, "State", "Complete");
    LCDLabelVal(2, "Progress", "100%");
    EnsureBarGlyphs();
    LCDBar(3, 0, LCD_COLS, 1, 1);          /* done==total -> full bar */
}

static void PageClock(void) {
    SysClock c; char v[20], n[12]; int p;
    Sys_GetClock(&c);
    LCDHeader("Clock");
    /* date: YYYY-MM-DD */
    p = 0; IToA(c.year, n); LCopy(v, sizeof(v), n); p = LL(v);
    v[p++] = '-'; v[p++] = (char)('0' + (c.mon / 10)); v[p++] = (char)('0' + (c.mon % 10));
    v[p++] = '-'; v[p++] = (char)('0' + (c.day / 10)); v[p++] = (char)('0' + (c.day % 10)); v[p] = 0;
    LCDLabelVal(1, "Date", v);
    /* time: HH:MM:SS */
    p = 0;
    v[p++] = (char)('0' + (c.hour / 10)); v[p++] = (char)('0' + (c.hour % 10)); v[p++] = ':';
    v[p++] = (char)('0' + (c.min / 10));  v[p++] = (char)('0' + (c.min % 10));  v[p++] = ':';
    v[p++] = (char)('0' + (c.sec / 10));  v[p++] = (char)('0' + (c.sec % 10));  v[p] = 0;
    LCDLabelVal(2, "Time", v);
    LCDGoto(3, 0); LCDPuts("", LCD_COLS);
}

static void DrawPage(int pageBit) {
    switch (pageBit) {
    case LCD_PAGE_TEMPS: PageTemps(); break;
    case LCD_PAGE_MEM:   PageMem();   break;
    case LCD_PAGE_DISK:  PageDisk(0); break;
    case LCD_PAGE_DISK2: PageDisk(1); break;
    case LCD_PAGE_NET:   PageNet();   break;
    case LCD_PAGE_FTP:   PageFtp();   break;
    case LCD_PAGE_CLOCK: PageClock(); break;
    default: PageSplash(); break;
    }
}

/* ---- re-probe + init at the current address ----------------------------- */
static void ProbeAndInit(void) {
    s_present = 0;
    s_addr8 = (s_cfg.addrChoice == LCD_ADDR_3D) ? LCD_ADDR8_3D : LCD_ADDR8_3C;
    if (!s_cfg.enabled) return;
    if (!SmbProbe()) return;             /* nothing at this address */
    LCDHardwareInit();
    ShadowInvalidate();
    Sleep(10);
    LcdSetContrastHW(0);                  /* start dark so the splash fades in */
    PageSplash();
    LcdFadeTo((BYTE)s_cfg.brightness);    /* smooth power-on fade-up */
    s_pageTimer = GetTickCount();
    s_sensorTimer = GetTickCount();
    s_present = 1;
}

/* ---- public ------------------------------------------------------------- */
void Lcd_Init(void) {
    CfgLoad();
    RebuildPageOrder();
    s_pageCur = 0;
    ProbeAndInit();
}

void Lcd_Tick(void) {
    DWORD now;
    if (!s_present || !s_cfg.enabled) return;

    now = GetTickCount();

    /* refresh sensors ~1/sec (not every frame -- SMBus is slow) */
    if (now - s_sensorTimer >= 1000) {
        s_sensorTimer = now;
        s_sensorOK = (Sys_ReadTemps(&s_cpuC, &s_boardC) ? 1 : 0);
        if (!Sys_ReadFanPct(&s_fanPct)) s_fanPct = 0;
    }

    /* page rotation */
    if (s_pageN == 0) { RebuildPageOrder(); if (s_pageN == 0) return; }

    /* FTP auto-focus: while a transfer is in progress, pin the FTP page (if it's
       enabled) so a glance at the panel shows the transfer + bar instead of
       whatever the timer landed on. Rotation resumes once the transfer ends.
       After a transfer finishes we hold a "Complete" frame for a short linger so
       small files -- which finish in a single tick and would otherwise never be
       seen -- stay on screen briefly. */
    if (s_cfg.pages & LCD_PAGE_FTP) {
        static int   s_ftpWasXfer = 0;
        static DWORD s_ftpLingerUntil = 0;
        int fst = Ftp_Status();

        if (fst == 3) {
            s_ftpWasXfer = 1;
            DrawPage(LCD_PAGE_FTP);
            s_pageTimer = now;          /* don't advance rotation while pinned   */
            return;
        }
        if (s_ftpWasXfer) {             /* transfer just ended -> start linger   */
            s_ftpWasXfer = 0;
            s_ftpLingerUntil = now + 1500;   /* ~1.5s of "Complete"             */
        }
        if (s_ftpLingerUntil && (long)(now - s_ftpLingerUntil) < 0) {
            PageFtpComplete();
            s_pageTimer = now;          /* hold rotation during the linger       */
            return;
        }
        s_ftpLingerUntil = 0;
    }

    if (now - s_pageTimer >= (DWORD)s_cfg.intervalMs) {
        s_pageTimer = now;
        s_pageCur = (s_pageCur + 1) % s_pageN;
    }

    /* redraw current page every tick; the shadow buffer makes unchanged cells
       free, so live values (clock seconds, FTP progress) update smoothly */
    DrawPage(s_pageOrder[s_pageCur]);
}

void Lcd_Shutdown(void) {
    if (!s_present) return;
    LCDCmd(HD44780_CLEAR); Sleep(2);
    s_present = 0;
}

int Lcd_IsPresent(void) { return s_present; }

int  Lcd_Enabled(void) { if (!s_cfgLoaded)CfgLoad(); return s_cfg.enabled; }
void Lcd_SetEnabled(int on) {
    if (!s_cfgLoaded)CfgLoad();
    s_cfg.enabled = on ? 1 : 0;
    CfgSave();
    if (s_cfg.enabled) ProbeAndInit();
    else Lcd_Shutdown();
}

int  Lcd_AddrChoice(void) { if (!s_cfgLoaded)CfgLoad(); return s_cfg.addrChoice; }
void Lcd_SetAddrChoice(int choice) {
    if (!s_cfgLoaded)CfgLoad();
    s_cfg.addrChoice = (choice == LCD_ADDR_3D) ? LCD_ADDR_3D : LCD_ADDR_3C;
    CfgSave();
    ProbeAndInit();                     /* re-detect at the new address */
}

int  Lcd_Pages(void) { if (!s_cfgLoaded)CfgLoad(); return s_cfg.pages; }
void Lcd_SetPages(int mask) {
    if (!s_cfgLoaded)CfgLoad();
    s_cfg.pages = mask & LCD_PAGE_ALL;
    CfgSave();
    RebuildPageOrder();
}
void Lcd_TogglePage(int bit) {
    if (!s_cfgLoaded)CfgLoad();
    s_cfg.pages ^= (bit & LCD_PAGE_ALL);
    CfgSave();
    RebuildPageOrder();
}

int  Lcd_IntervalMs(void) { if (!s_cfgLoaded)CfgLoad(); return s_cfg.intervalMs; }
void Lcd_SetIntervalMs(int ms) {
    if (!s_cfgLoaded)CfgLoad();
    if (ms < 1000) ms = 1000;
    if (ms > 30000) ms = 30000;
    s_cfg.intervalMs = ms;
    CfgSave();
}

int Lcd_Brightness(void) { if (!s_cfgLoaded)CfgLoad(); return s_cfg.brightness; }

void Lcd_SetBrightness(int v) {
    if (!s_cfgLoaded)CfgLoad();
    if (v < 16) v = 16;            /* keep it visible -- never fully dark */
    if (v > 255) v = 255;
    s_cfg.brightness = v;
    if (s_present) LcdSetContrastHW((BYTE)v);   /* apply live */
    CfgSave();
}

void Lcd_NowPlaying(const char* title) {
    char l0[21], l1[21], l2[21], l3[21];
    char t1[21], t2[21];
    int len, i, take;

    if (!s_present || !s_cfg.enabled) return;
    if (!title) title = "";

    /* line 0: framed header. Kept to 17 chars so it centres with a margin on
       both sides -- the old 19-char ">>> NOW PLAYING <<<" exactly filled the
       interior (no left pad), so it hugged the left edge and read off-centre. */
    LCDCenter(l0, ">> NOW PLAYING <<");

    /* lines 2-3: the title, vertically centred and wrapped across two rows when
       long, so short names sit on one line with breathing room above/below and
       longer ones still read. Truncate at 40 chars total (2 x 20). */
    len = LL(title);
    if (len <= LCD_COLS) {
        LCDCenter(l1, "");
        LCDCenter(l2, title);
        LCDCenter(l3, "");
    }
    else {
        take = LCD_COLS;
        for (i = LCD_COLS; i > LCD_COLS - 6 && i > 0; i--) {
            if (title[i] == ' ') { take = i; break; }
        }
        { int n = 0; for (i = 0; i < take && n < 20; i++) t1[n++] = title[i]; t1[n] = 0; }
        {
            int n = 0, st = take; if (title[st] == ' ') st++;
            for (i = st; title[i] && n < 20; i++) t2[n++] = title[i]; t2[n] = 0;
        }
        LCDCenter(l1, "");
        LCDCenter(l2, t1);
        LCDCenter(l3, t2);
    }

    LCDGoto(0, 0); LCDPuts(l0, LCD_COLS);
    LCDGoto(1, 0); LCDPuts(l1, LCD_COLS);
    LCDGoto(2, 0); LCDPuts(l2, LCD_COLS);
    LCDGoto(3, 0); LCDPuts(l3, LCD_COLS);

    /* invalidate the shadow so that when we return to the dash, Lcd_Tick draws
       the next rotating page from scratch instead of diffing against this. */
    ShadowInvalidate();
}