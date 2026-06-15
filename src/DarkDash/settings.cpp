/*---------------------------------------------------------------------------
    Settings.cpp -- SETTINGS screen.

    Every category follows the same design language as the main menu and the
    Games/Homebrew launcher: the chosen category's icon spins on the pedestal
    (left) and its options live as rows in the tilted iso console (right),
    with the same green bloom and selection glow. About is the one exception
    -- it's a framed vertical auto-scroller. Nothing writes the EEPROM; only
    dd_data files, the runtime SMC fan register, and the system clock are
    ever written.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <string.h>
#include "dd_gfx.h"
#include "dd_ui.h"
#include "dd_iso.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "font.h"
#include "input.h"
#include "dd_audio.h"
#include "dd_data.h"
#include "dd_sysinfo.h"
#include "dd_eeprom.h"
#include "dd_time.h"
#include "dd_ntp.h"
#include "dd_net.h"
#include "dd_pedestal.h"
#include "dd_backdrop.h"
#include "dd_osk.h"
#include "dd_ftp.h"
#include "dd_lcd.h"
#include "dd_typed.h"
#include "dd_xview.h"
#include "xv_protocol.h"   /* XV_PANEL_A / XV_PANEL_B */
#include "dd_typedart.h"
#include "dd_launcher.h"
#include "dd_udp.h"
#include "dd_rgb.h"
#include "dd_oxfp.h"
#include "dd_dc.h"
#include "dd_update.h"
#include "dd_rtc.h"
#include "dd_calib.h"
#include "dd_select.h"
#include "Settings.h"

/* forward decls for the download render-pump (defined after Settings_Render) */
void Settings_Render(void);
static void RenderAccessories(IDirect3DDevice8* d);
static void SettingsUpdRenderPump(void);
#include "dd_version.h"

#define SET_COUNT 11
#define MUSIC_DIR "D:\\audio\\music"
#define MUSIC_MAX 64

enum {
    CAT_NETWORK = 0, CAT_FTP, CAT_VIDEO, CAT_AUDIO,
    CAT_FAN, CAT_ACCESSORIES, CAT_CLOCK, CAT_THEME, CAT_FONT, CAT_UPDATE, CAT_ABOUT
};

static const char* const k_items[SET_COUNT] = {
    "Network", "FTP", "Video", "Audio", "Fan", "Accessories", "Clock", "Theme", "Font", "Update", "About"
};
static const char* const k_icon[SET_COUNT] = {
    "s2_020.png", "s2_018.png", "s2_005.png", "s2_004.png", "s2_010.png",
    "s2_013.png", "s2_008.png", "s2_006.png", "s2_002.png", "s2_019.png", "s2_011.png"
};

static int     s_cursor = 0;     /* category list cursor       */
static int     s_view = -1;    /* open category, or -1 = list */
static int     s_row = 0;     /* selected option row in a panel */
static Texture s_icon = { 0, 0, 0, 0, 0 };
static int     s_iconIdx = -1;

/* Audio: 0 = options, 1 = MP3 picker */
static int  s_audioPick = 0;
static int  s_audioMsg = 0;    /* 0 none, 1 saved, 2 failed (EEPROM audio) */
static char s_mp3[MUSIC_MAX][64];
static int  s_mp3Count = 0;
static int  s_mp3Cursor = 0;

/* Fan working copy (committed to dd_data on B) */
static int  s_fanAuto = 1;
static int  s_fanPct = 50;

/* Clock working copy (read on open, applied on the Set row) */
static SysClock s_clk = { 2026, 1, 1, 0, 0, 0, 0 };
static int      s_clkMsg = 0;     /* 0 none, 1 set ok, 2 set failed */
static int      s_clkPart = 0;    /* Time field sub-cursor: 0 = hour, 1 = minute */

/* Network working copy (read on open via XNetLoadConfigParams) */
static int           s_netMode = DD_NET_DHCP;
static unsigned long s_netIp = 0, s_netMask = 0, s_netGw = 0, s_netDns1 = 0, s_netDns2 = 0;
static int           s_netEdit = -1;   /* field id 1..5 currently in the OSK, -1 none */
static int           s_netMsg = 0;    /* 0 none, 1 applied, 2 failed */

/* FTP OSK edit target: 0 none, 1 port, 2 user, 3 pass */
static int           s_ftpEdit = 0;

/* Font panel: scanned .ddf names (no extension) + cursor + apply message */
#define FONT_DIR   "D:\\fonts"
#define FONT_MAX   64
static char s_fonts[FONT_MAX][64];
static int  s_fontCount = 0;
static int  s_fontCursor = 0;
static int  s_fontMsg = 0;       /* 0 none, 1 applied, 2 failed -> Default */

/* About panel: tick when the screen was entered, so the scroller starts at top */
static DWORD s_aboutEnter = 0;

/* Video panel Effects sub-menu */
static int s_fxSub = 0;    /* 0 = Video rows, 1 = Effects sub-menu */

/* Accessories: two-level. s_accDev = -1 -> device list; else a device screen.
   s_accRow = cursor within the active level. OXFP/RGB rows grey out and do
   nothing when their device isn't currently detected (Udp_Present). */
enum { ACC_DEV_LCD = 0, ACC_DEV_TYPED, ACC_DEV_RGB, ACC_DEV_OXFP, ACC_DEV_XVIEW, ACC_DEV_COUNT };
static int s_accDev = -1;
static int s_accRow = 0;

/* On entering the RGB/OXFP screen we pull the device's CURRENT config over UDP
   and adopt it (so the menu reflects the live device, not dc.dat defaults).
   One-shot per entry so it never fights the user's live adjustments. */
static int   s_accSync = 0;          /* 1 = a pull is in progress for s_accDev  */
static DWORD s_accSyncStart = 0;     /* entry time: only accept replies newer    */
static DWORD s_accSyncSent = 0;      /* last "get" send (for periodic re-ask)    */

/* live working values for the RGB / OXFP control screens (sent as we adjust).
   colors are palette indices (into k_palette) so L/R scrolls presets. */
static int s_rgbMode = 0, s_rgbBright = 128, s_rgbSpeed = 128, s_rgbIntensity = 128;
static int s_rgbPalCount = 2;
static int s_rgbColIx[4] = { 0, 1, 2, 3 };       /* colorA..D -> palette index  */

static int s_oxfpMode = 0, s_oxfpBright = 128;
static int s_oxfpAnim = 0, s_oxfpAnimSpeed = 128;
static int s_oxfpStatusIx[3] = { 4, 6, 8 };      /* green/red/orange -> palette  */
static int s_oxfpAnimIx[2] = { 0, 2 };         /* animA/animB -> palette       */

/* shared preset palette the color slots scroll through (0xRRGGBB) */
static const unsigned long k_palette[] = {
    0xFF0000UL, 0xFF4000UL, 0xFF8000UL, 0xFFC000UL,   /* reds -> amber           */
    0x00FF00UL, 0x00FF80UL, 0x00FFC0UL, 0x00FFFFUL,   /* greens -> cyan          */
    0x0000FFUL, 0x4000FFUL, 0x8000FFUL, 0xFF00FFUL,   /* blues -> magenta        */
    0xFFFFFFUL, 0xFFC0A0UL, 0x80FF00UL, 0xFFE000UL    /* white, warm, lime, gold */
};
#define PALETTE_COUNT ((int)(sizeof(k_palette)/sizeof(k_palette[0])))

/* map an arbitrary device RGB to the closest swatch in k_palette (the menus
   only carry palette indices, so a read-back color snaps to the nearest preset). */
static int NearestPalette(int r, int g, int b) {
    int i, best = 0;
    long bestD = 0x7FFFFFFFL;
    for (i = 0; i < PALETTE_COUNT; i++) {
        int pr = (int)((k_palette[i] >> 16) & 0xFF);
        int pg = (int)((k_palette[i] >> 8) & 0xFF);
        int pb = (int)(k_palette[i] & 0xFF);
        long dr = (long)pr - r, dg = (long)pg - g, db = (long)pb - b;
        long d = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}
static int s_fxRow = 0;    /* cursor within the Effects sub-menu   */
static const int k_fxBits[6] = { DD_FX_SCANLINES, DD_FX_SELECT, DD_FX_IDLE, DD_FX_EDGE, DD_FX_PLASMA, DD_FX_ARCS };
static const char* const k_fxNames[6] = { "Scanlines", "Selection FX", "Idle Motion", "Edge Flash", "Plasma Orb", "Frame Arcs" };

/* Theme panel: discovered theme folder names + cursor + apply message */
#define THEMES_DIR "D:\\themes"
static int  s_themeCursor = 0;
static int  s_themeMsg = 0;      /* 0 none, 1 applied, 2 failed -> default */

/*---- helpers ---------------------------------------------------------------*/

static void LoadIcon(int idx) {
    char path[260];
    if (idx == s_iconIdx) return;
    if (idx < 0 || idx >= SET_COUNT) return;
    if (s_icon.tex) Texture_Release(&s_icon);
    s_icon.tex = NULL;
    /* prefer the active theme's icon, fall back to default if it doesn't ship it */
    Theme_ResolveIcon(k_icon[idx], path, sizeof(path));
    Texture_LoadPNG(path, &s_icon);
    s_iconIdx = idx;
}
static void ReleaseIcon(void) {
    if (s_icon.tex) Texture_Release(&s_icon);
    s_icon.tex = NULL; s_iconIdx = -1;
}

static int IntToText(int v, char* out) {
    char tmp[12]; int n = 0, i, t = v;
    if (t == 0) tmp[n++] = '0';
    while (t > 0) { tmp[n++] = (char)('0' + (t % 10)); t /= 10; }
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

/* two-digit, zero-padded (e.g. 6 -> "06") */
static void Pad2(int v, char* out) {
    char n[8];
    out[0] = 0;
    if (v < 10) { out[0] = '0'; out[1] = 0; }
    IntToText(v, n);
    strcat(out, n);
}

/* packed network-order IPv4 <-> "a.b.c.d" (byte0 = first octet, matches IN_ADDR) */
static void FmtIp(unsigned long v, char* out) {
    BYTE* b = (BYTE*)&v;
    char* p = out;
    int   i, n;
    for (i = 0; i < 4; i++) {
        int d = b[i];
        char t[4]; n = 0;
        if (d == 0) t[n++] = '0';
        while (d > 0) { t[n++] = (char)('0' + (d % 10)); d /= 10; }
        while (n > 0) *p++ = t[--n];
        if (i < 3) *p++ = '.';
    }
    *p = 0;
}
static unsigned long ParseIp(const char* s) {
    unsigned long v = 0;
    int oct = 0, cur = 0, shift = 0;
    const char* p = s;
    for (;;) {
        if (*p >= '0' && *p <= '9') { cur = cur * 10 + (*p - '0'); if (cur > 255) cur = 255; }
        else if (*p == '.' || *p == 0) {
            v |= ((unsigned long)(cur & 0xFF)) << shift;
            shift += 8; oct++; cur = 0;
            if (*p == 0 || oct >= 4) break;
        }
        p++;
    }
    return v;
}
static unsigned long* NetField(int id) {
    switch (id) {
    case 1: return &s_netIp;   case 2: return &s_netMask;  case 3: return &s_netGw;
    case 4: return &s_netDns1; case 5: return &s_netDns2;
    }
    return 0;
}

static void ScanMp3(void) {
    char pattern[160]; WIN32_FIND_DATA fd; HANDLE h;
    s_mp3Count = 0;
    strcpy(pattern, MUSIC_DIR); strcat(pattern, "\\*.mp3");
    h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (s_mp3Count >= MUSIC_MAX) break;
        strncpy(s_mp3[s_mp3Count], fd.cFileName, 63); s_mp3[s_mp3Count][63] = 0;
        s_mp3Count++;
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

/* case-insensitive match to the built-in default track name */
static int IsBgTrack(const char* n) {
    static const char* b = "bg.mp3";
    int i;
    for (i = 0; b[i]; i++) {
        char c = n[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != b[i]) return 0;
    }
    return n[i] == 0;
}

/* Pick a random index into s_mp3 for Shuffle, EXCLUDING the built-in bg.mp3
   (it lives in the music dir but isn't a "song") and avoiding an immediate
   repeat of the last track. Returns -1 when there's nothing real to shuffle. */
static int s_lastShufIdx = -1;
static unsigned long s_shufSeed = 0;
static int PickShuffleIdx(void) {
    int pool[MUSIC_MAX], np = 0, i, choice;
    for (i = 0; i < s_mp3Count; i++)
        if (!IsBgTrack(s_mp3[i])) pool[np++] = i;
    if (np == 0) return -1;
    if (np == 1) return pool[0];
    s_shufSeed = s_shufSeed * 1103515245UL + 12345UL;
    s_shufSeed ^= (unsigned long)GetTickCount() << 13;
    choice = (int)((s_shufSeed >> 11) % (unsigned long)np);
    if (pool[choice] == s_lastShufIdx) choice = (choice + 1) % np;   /* no immediate repeat */
    return pool[choice];
}

/* --- "Now Playing" toast: armed whenever Shuffle starts a real track; drawn
   under the pedestal by the splash and faded out by Settings_NowPlaying(). --- */
static char  s_npName[64];
static DWORD s_npStart = 0;        /* GetTickCount at toast start; 0 = inactive */
#define NP_HOLD_MS 10000          /* full-opacity hold                          */
#define NP_FADE_MS 900             /* fade-out tail                              */

/* Persistent "current shuffle track" state for the LCD Now-Playing page. Unlike
   the toast above (s_npStart, which self-zeroes after the fade), this holds for
   the whole track so the panel can show a live elapsed time. Cleared whenever
   playback stops or the mode leaves Shuffle. */
static char  s_curTrack[64];
static DWORD s_curTrackStart = 0;  /* GetTickCount when the track began; 0 = none */

static void NowPlayingSet(const char* file) {
    int i;
    for (i = 0; i < 63 && file[i]; i++) s_npName[i] = file[i];
    s_npName[i] = 0;
    /* drop a trailing .mp3 / .MP3 so the toast shows a clean name */
    if (i >= 4) {
        char* e = s_npName + i - 4;
        if (e[0] == '.' && (e[1] == 'm' || e[1] == 'M') &&
            (e[2] == 'p' || e[2] == 'P') && e[3] == '3')
            e[0] = 0;
    }
    s_npStart = GetTickCount();
    if (s_npStart == 0) s_npStart = 1; /* reserve 0 as the "inactive" sentinel */

    /* mirror into the persistent LCD state (survives the toast fade) */
    for (i = 0; i < 63 && s_npName[i]; i++) s_curTrack[i] = s_npName[i];
    s_curTrack[i] = 0;
    s_curTrackStart = s_npStart;
}

/* Resolve the saved music choice and (re)start playback. One place so boot and
   the picker behave identically. NONE stops playback; SHUFFLE picks a random
   .mp3 from the music dir (falling back to built-in if none); NORMAL plays the
   custom file when set, otherwise the built-in bg track. */
void Settings_StartMusic(int loop) {
    DD_Settings* st = Data_Get();
    Audio_SetMusicVolume(st->musicVolume);

    if (st->musicMode == DD_MUSIC_NONE) {
        Audio_StopMusic();
        s_npStart = 0;
        s_curTrackStart = 0;
        return;
    }

    if (st->musicMode == DD_MUSIC_SHUFFLE) {
        ScanMp3();
        Audio_StopMusic();
        {
            int idx = PickShuffleIdx();
            if (idx >= 0) {
                char full[260];
                s_lastShufIdx = idx;
                strcpy(full, MUSIC_DIR); strcat(full, "\\"); strcat(full, s_mp3[idx]);
                Audio_SetMusicPath(full);
                Audio_StartMusic(0);         /* NON-looping: the tick rolls the next track on end */
                NowPlayingSet(s_mp3[idx]);   /* arm the under-pedestal toast */
            }
            else {
                Audio_SetMusicPath(0);       /* no real songs -> built-in, looped */
                Audio_StartMusic(1);
                s_npStart = 0;               /* nothing real to announce */
                s_curTrackStart = 0;
            }
        }
        return;
    }

    /* DD_MUSIC_NORMAL: custom file if one is set, else the built-in track */
    Audio_StopMusic();
    Audio_SetMusicPath((st->musicCustom && st->musicPath[0]) ? st->musicPath : 0);
    Audio_StartMusic(loop);
    s_npStart = 0;
    s_curTrackStart = 0;
}

/* Per-frame music pump. Only Shuffle needs it: when the current (non-looping)
   track has played to its end, roll the next random track. Cheap no-op in every
   other mode. Call once per frame from the main loop. */
void Settings_MusicTick(void) {
    if (Data_Get()->musicMode != DD_MUSIC_SHUFFLE) return;
    if (Audio_MusicFinished()) Settings_StartMusic(0);   /* pick + start the next track */
}

/* Drawn under the pedestal: full opacity for NP_HOLD_MS after a track change,
   then a short fade. Self-disarms once the fade completes. */
int Settings_NowPlaying(const char** name, float* alpha) {
    DWORD el;
    if (s_npStart == 0) return 0;
    el = GetTickCount() - s_npStart;
    if (el >= (DWORD)(NP_HOLD_MS + NP_FADE_MS)) { s_npStart = 0; return 0; }
    *name = s_npName;
    if (el <= (DWORD)NP_HOLD_MS) *alpha = 1.0f;
    else *alpha = 1.0f - (float)(el - (DWORD)NP_HOLD_MS) / (float)NP_FADE_MS;
    return 1;
}

/* Live shuffle "Now Playing" for the LCD page: 1 while Shuffle is playing a real
   track, filling *name (cleaned, .mp3 stripped) and *elapsedMs (since the track
   began). Returns 0 in every other music mode / when nothing real is playing, so
   the LCD page hides itself outside Shuffle. name/elapsedMs may be NULL. */
int Settings_ShuffleNowPlaying(const char** name, DWORD* elapsedMs) {
    if (Data_Get()->musicMode != DD_MUSIC_SHUFFLE) return 0;
    if (s_curTrackStart == 0) return 0;
    if (name)      *name = s_curTrack;
    if (elapsedMs) *elapsedMs = GetTickCount() - s_curTrackStart;
    return 1;
}

/* scan D:\fonts for *.ddf; index 0 is always "Default" (baked failsafe), then
   each file with its ".ddf" extension stripped off. */
static void ScanFonts(void) {
    char pattern[160]; WIN32_FIND_DATA fd; HANDLE h;
    s_fontCount = 0;
    strcpy(s_fonts[s_fontCount++], "Default");

    strcpy(pattern, FONT_DIR); strcat(pattern, "\\*.ddf");
    h = FindFirstFile(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (s_fontCount >= FONT_MAX) break;
            strncpy(s_fonts[s_fontCount], fd.cFileName, 63);
            s_fonts[s_fontCount][63] = 0;
            /* strip the trailing ".ddf" */
            n = 0; while (s_fonts[s_fontCount][n]) n++;
            if (n > 4) s_fonts[s_fontCount][n - 4] = 0;
            s_fontCount++;
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
}

/* shared chrome: backdrop + header + footer (same on every screen) */
static void Chrome(IDirect3DDevice8* d, const char* title, const char* foothint) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    const Texture* hdr = Theme_Asset("bar_header");
    const Texture* foot = Theme_Asset("bar_footer");
    Backdrop_Draw();
    if (hdr) UI_DrawSprite(hdr, 8.0f, 8.0f, 300.0f, 40.0f, 0xFFFFFFFF, 0);
    Font_DrawText(d, 26.0f, 14.0f, title, FONT_SIZE_LARGE, accent, 0);
    if (foot) UI_DrawSprite(foot, 8.0f, 442.0f, 624.0f, 32.0f, 0xFFFFFFFF, 0);
    Font_DrawText(d, 24.0f, 449.0f, foothint, FONT_SIZE_SMALL, text, 0);
}

/* the pedestal + spinning category icon (left), same as the launcher cube */
static void DrawPedestal(IDirect3DDevice8* d) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* ped = Theme_Asset("platform_round");
    (void)d;
    if (ped) UI_DrawSprite(ped, 70.0f, 270.0f, 220.0f, 116.0f, 0xFFFFFFFF, 0);
    Pedestal_DrawFlat(s_icon.tex ? &s_icon : NULL, GetTickCount(), ar, ag, ab);
}

/* the tilted console (right) holding option rows. selRow highlighted; rows
   past 'selectable' are shown dim (read-only). If pinLast, the final row is
   drawn at the bottom of the frame (used for an "Apply"/action row). */
static void DrawConsole(IDirect3DDevice8* d, const char* const* rows, int count,
    int selRow, int selectable, int pinLast, DWORD disabledMask = 0) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* menu = Theme_Asset("frame_menu_v");
    float menuX = 352.0f, menuY = 48.0f, rowY0 = 96.0f, rowDY = 42.0f;
    float bottomY = menuY + 384.0f - 50.0f;   /* pinned action row, near frame foot */
    int i;
    /* How many rows fit between rowY0 and the pinned-action row. Scrolling stays
       a no-op when the list fits (count <= visRows) -- so existing short screens
       are unchanged -- and only engages for tall ones like the LCD page list.
       The pinned last row (if any) is always drawn at the foot, so it's excluded
       from the scrolling window. */
    int hasPin = (pinLast && count > 0) ? 1 : 0;
    int scrollCount = count - hasPin;                 /* rows that participate in scroll */
    /* Usable height: down to the pinned-action row when one exists, otherwise
       all the way to the frame's bottom inset -- so screens without an action
       row (like the LCD page list) use the whole panel instead of leaving the
       lower half empty. */
    float listBottom = hasPin ? bottomY : (menuY + 384.0f - 24.0f);
    int visRows;
    static int s_consoleScroll = 0;
    int first;

    /* Adaptive row spacing: keep the comfortable 42px default for short screens,
       but if a screen has more rows than fit at that pitch, tighten just enough
       to show them all in the whole frame (down to a 30px floor) rather than
       scrolling. The LCD page list (9 rows) becomes fully visible this way. */
    {
        int fitAt42 = (int)((listBottom - rowY0) / rowDY);
        if (scrollCount > fitAt42 && scrollCount > 0) {
            float dy = (listBottom - rowY0) / (float)scrollCount;
            if (dy < 30.0f) dy = 30.0f;
            rowDY = dy;
        }
    }
    visRows = (int)((listBottom - rowY0) / rowDY);
    if (visRows < 1) visRows = 1;

    /* keep the selected (non-pinned) row within the window */
    if (selRow >= 0 && !(hasPin && selRow == count - 1)) {
        if (selRow < s_consoleScroll) s_consoleScroll = selRow;
        if (selRow >= s_consoleScroll + visRows) s_consoleScroll = selRow - visRows + 1;
    }
    if (s_consoleScroll > scrollCount - visRows && scrollCount > visRows) s_consoleScroll = scrollCount - visRows;
    if (scrollCount <= visRows) s_consoleScroll = 0;
    if (s_consoleScroll < 0) s_consoleScroll = 0;
    first = s_consoleScroll;

    Iso_Begin();
    if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
    if (selRow >= 0 && (selRow < selectable || (pinLast && selRow == count - 1))
        && !(disabledMask & (1u << selRow))) {
        float gy;
        if (pinLast && selRow == count - 1) gy = bottomY - 6.0f;
        else                                gy = rowY0 + rowDY * (float)(selRow - first) - 6.0f;
        Select_Begin(0x5000 + s_view, gy);
        Select_DrawGlow(menuX + 18.0f, gy, 210.0f, 34.0f, UI_ARGB(110, ar, ag, ab));
    }
    for (i = 0; i < count; i++) {
        int isAction = (pinLast && i == count - 1);
        int disabled = (disabledMask & (1u << i)) ? 1 : 0;
        DWORD c;
        float ry;
        if (isAction)        c = text;        /* pinned action: always live */
        else if (disabled)        c = dim;         /* greyed: not selectable      */
        else if (i == selRow)     c = glow;
        else if (i >= selectable) c = dim;
        else                      c = text;
        if (isAction) {
            ry = bottomY;                               /* pinned at the foot */
        }
        else {
            int vis = i - first;                        /* position in window */
            if (vis < 0 || vis >= visRows) continue;    /* scrolled out of view */
            ry = rowY0 + rowDY * (float)vis;
        }
        Font_DrawTextIsoClip(d, menuX + 28.0f, ry + 4.0f, rows[i], FONT_SIZE_MEDIUM, c, 224.0f);
    }
    Iso_End();
}

/*---- enter -----------------------------------------------------------------*/

void Settings_Enter(void) {
    DD_Settings* st = Data_Get();
    s_cursor = 0; s_view = -1; s_row = 0; s_iconIdx = -1; s_audioPick = 0;
    s_fanAuto = st->fanAuto; s_fanPct = st->fanPercent;
    Select_Reset();
    LoadIcon(0);
}

/*---- input -----------------------------------------------------------------*/

static int UpdateList(WORD pressed) {
    if (pressed & BTN_B) { Audio_PlaySfx(SFX_BACK); ReleaseIcon(); return 1; }
    if (pressed & BTN_DPAD_DOWN) { if (s_cursor < SET_COUNT - 1) { s_cursor++; Audio_PlaySfx(SFX_NAV_DOWN); LoadIcon(s_cursor); } }
    if (pressed & BTN_DPAD_UP) { if (s_cursor > 0) { s_cursor--; Audio_PlaySfx(SFX_NAV_UP);   LoadIcon(s_cursor); } }
    if (pressed & BTN_A) {
        Audio_PlaySfx(SFX_SELECT);
        s_view = s_cursor; s_row = 0; s_audioPick = 0; s_audioMsg = 0;
        s_fxSub = 0;   /* video Effects sub-menu starts closed */
        s_accDev = -1; s_accRow = 0; s_accSync = 0;   /* accessories starts at the device list */
        Select_Reset();   /* snap the highlight to the new panel's first row */
        if (s_view == CAT_CLOCK) { Sys_GetClock(&s_clk); s_clkMsg = 0; }
        if (s_view == CAT_NETWORK) {
            Net_LoadConfig(&s_netMode, &s_netIp, &s_netMask, &s_netGw, &s_netDns1, &s_netDns2);
            s_netEdit = -1; s_netMsg = 0;
        }
        if (s_view == CAT_FONT) {
            int i;
            DD_Settings* st = Data_Get();
            ScanFonts();
            s_fontMsg = 0; s_fontCursor = 0;
            /* land the cursor on the saved font if it's in the list */
            if (st->fontName[0]) {
                for (i = 1; i < s_fontCount; i++)
                    if (lstrcmpA(s_fonts[i], st->fontName) == 0) { s_fontCursor = i; break; }
            }
        }
        if (s_view == CAT_UPDATE) {
            Upd_Init(DARKDASH_VERSION);
            Upd_StartCheck();
        }
        if (s_view == CAT_ABOUT) {
            s_aboutEnter = GetTickCount();   /* start the scroller at the top */
        }
        if (s_view == CAT_THEME) {
            int i, n;
            DD_Settings* st = Data_Get();
            n = Theme_Scan(THEMES_DIR);
            s_themeMsg = 0; s_themeCursor = 0;
            /* land the cursor on the saved theme (empty name = "default" at 0) */
            if (st->themeName[0]) {
                for (i = 0; i < n; i++) {
                    const char* nm = Theme_NameAt(i);
                    if (nm && lstrcmpiA(nm, st->themeName) == 0) { s_themeCursor = i; break; }
                }
            }
        }
    }
    return 0;
}

static void UpdateAudio(WORD pressed) {
    DD_Settings* st = Data_Get();
    if (s_audioPick) {
        /* combined picker: row 0 = None, row 1 = Shuffle, then the .mp3 files */
        int pickCount = 2 + s_mp3Count;
        if (pressed & BTN_DPAD_DOWN) { if (s_mp3Cursor < pickCount - 1) { s_mp3Cursor++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_mp3Cursor > 0) { s_mp3Cursor--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & BTN_A) {
            if (s_mp3Cursor == 0) {                  /* None */
                st->musicMode = DD_MUSIC_NONE;
                Settings_StartMusic(1); Data_Save(); Audio_PlaySfx(SFX_SELECT);
            }
            else if (s_mp3Cursor == 1) {             /* Shuffle */
                st->musicMode = DD_MUSIC_SHUFFLE;
                Settings_StartMusic(1); Data_Save(); Audio_PlaySfx(SFX_SELECT);
            }
            else {                                   /* a specific file */
                int fi = s_mp3Cursor - 2;
                if (fi >= 0 && fi < s_mp3Count) {
                    char full[260];
                    strcpy(full, MUSIC_DIR); strcat(full, "\\"); strcat(full, s_mp3[fi]);
                    st->musicMode = DD_MUSIC_NORMAL;
                    st->musicCustom = 1;
                    strncpy(st->musicPath, full, DD_MUSIC_PATH_MAX - 1); st->musicPath[DD_MUSIC_PATH_MAX - 1] = 0;
                    Settings_StartMusic(1); Data_Save(); Audio_PlaySfx(SFX_SELECT);
                }
            }
            s_audioPick = 0;
        }
        if (pressed & BTN_X) {                        /* shortcut: built-in default */
            st->musicMode = DD_MUSIC_NORMAL;
            st->musicCustom = 0; st->musicPath[0] = 0;
            Settings_StartMusic(1); Data_Save(); Audio_PlaySfx(SFX_ALT); s_audioPick = 0;
        }
        return;
    }
    if (pressed & BTN_DPAD_DOWN) { if (s_row < 4) { s_row++; Audio_PlaySfx(SFX_NAV_DOWN); } }
    if (pressed & BTN_DPAD_UP) { if (s_row > 0) { s_row--; Audio_PlaySfx(SFX_NAV_UP); } }
    if (s_row == 0 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
        int v = st->musicVolume;
        if (pressed & BTN_DPAD_LEFT)  v -= 5;
        if (pressed & BTN_DPAD_RIGHT) v += 5;
        if (v < 0) v = 0; if (v > 100) v = 100;
        st->musicVolume = v; Audio_SetMusicVolume(v); Audio_PlaySfx(SFX_NAV_UP);
    }
    if (s_row == 1 && (pressed & BTN_A)) { ScanMp3(); s_mp3Cursor = 0; s_audioPick = 1; Audio_PlaySfx(SFX_SELECT); }

    /* console audio config (EEPROM-backed). Read current, modify, persist via
       the kernel single-setting write. Games may need a restart to pick it up. */
    if (s_row == 2 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
        int m = 0, a = 0, t = 0;
        Eeprom_GetAudio(&m, &a, &t);
        if (pressed & BTN_DPAD_RIGHT) m = (m + 1) % 3;
        else                          m = (m + 2) % 3;
        s_audioMsg = Eeprom_SetAudio(m, a, t) ? 1 : 2;
        Audio_PlaySfx(SFX_ALT);
    }
    if (s_row == 3 && (pressed & (BTN_A | BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
        int m = 0, a = 0, t = 0;
        Eeprom_GetAudio(&m, &a, &t);
        s_audioMsg = Eeprom_SetAudio(m, !a, t) ? 1 : 2;
        Audio_PlaySfx(SFX_ALT);
    }
    if (s_row == 4 && (pressed & (BTN_A | BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
        int m = 0, a = 0, t = 0;
        Eeprom_GetAudio(&m, &a, &t);
        s_audioMsg = Eeprom_SetAudio(m, a, !t) ? 1 : 2;
        Audio_PlaySfx(SFX_ALT);
    }
}

static void UpdateFan(WORD pressed) {
    int selectable = s_fanAuto ? 1 : 2;
    if (pressed & BTN_DPAD_DOWN) { if (s_row < selectable - 1) { s_row++; Audio_PlaySfx(SFX_NAV_DOWN); } }
    if (pressed & BTN_DPAD_UP) { if (s_row > 0) { s_row--; Audio_PlaySfx(SFX_NAV_UP); } }
    if (s_row == 0 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT | BTN_A))) {
        s_fanAuto = !s_fanAuto; Audio_PlaySfx(SFX_ALT);
        if (s_fanAuto) { Sys_FanAuto(); s_row = 0; }
        else           Sys_FanSetManual(s_fanPct);
    }
    if (!s_fanAuto && s_row == 1 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
        if (pressed & BTN_DPAD_LEFT)  s_fanPct -= 5;
        if (pressed & BTN_DPAD_RIGHT) s_fanPct += 5;
        if (s_fanPct < 20)  s_fanPct = 20;
        if (s_fanPct > 100) s_fanPct = 100;
        Sys_FanSetManual(s_fanPct); Audio_PlaySfx(SFX_NAV_UP);
    }
}

static void ClampClock(void) {
    if (s_clk.year < 2000) s_clk.year = 2000; if (s_clk.year > 2099) s_clk.year = 2099;
    if (s_clk.mon < 1)    s_clk.mon = 12;   if (s_clk.mon > 12)   s_clk.mon = 1;
    if (s_clk.day < 1)    s_clk.day = 31;   if (s_clk.day > 31)   s_clk.day = 1;
    if (s_clk.hour < 0)    s_clk.hour = 23;   if (s_clk.hour > 23)   s_clk.hour = 0;
    if (s_clk.min < 0)    s_clk.min = 59;   if (s_clk.min > 59)   s_clk.min = 0;
}

static int NetRowCount(void) {
    return (s_netMode == DD_NET_STATIC) ? 7 : (s_netMode == DD_NET_DHCP_DNS) ? 4 : 2;
}
/* row -> kind: 0 = Mode, 1..5 = field id (IP/Subnet/Gateway/DNS1/DNS2), 6 = Apply */
static int NetRowKind(int row) {
    if (row == 0) return 0;
    if (s_netMode == DD_NET_STATIC)  return (row <= 5) ? row : 6;
    if (s_netMode == DD_NET_DHCP_DNS) { if (row == 1) return 4; if (row == 2) return 5; return 6; }
    return 6;   /* DHCP: row1 = Apply */
}

static void FtpApply(void) {
    /* restart the service to pick up new port/creds. Ftp_Want defers the actual
       bind to Ftp_Tick once the network is ready (and re-binds with new creds). */
    DD_Settings* st = Data_Get();
    Data_Save();
    Ftp_Stop();
    if (st->ftpEnabled)
        Ftp_Want(1);
}

static void UpdateUpdate(WORD pressed) {
    int st = Upd_State();

    /* always advance the network/download state machine */
    Upd_Tick();

    if (pressed & BTN_A) {
        switch (st) {
        case UPD_IDLE:
        case UPD_ERROR:
        case UPD_UPTODATE:   Upd_StartCheck();    Audio_PlaySfx(SFX_SELECT); break;
        case UPD_AVAILABLE:  Upd_SetRenderFn(SettingsUpdRenderPump); Upd_StartDownload(); Audio_PlaySfx(SFX_SELECT); break;
        case UPD_DONE:       Upd_Relaunch();       break;   /* never returns */
        default: break;      /* CHECKING/DOWNLOADING/EXTRACTING: ignore */
        }
    }
}

static void UpdateTheme(WORD pressed) {
    DD_Settings* st = Data_Get();
    int count = Theme_Count();
    if (pressed & BTN_DPAD_DOWN) { if (s_themeCursor < count - 1) { s_themeCursor++; Audio_PlaySfx(SFX_NAV_DOWN); } }
    if (pressed & BTN_DPAD_UP) { if (s_themeCursor > 0) { s_themeCursor--; Audio_PlaySfx(SFX_NAV_UP); } }

    if (pressed & BTN_A) {
        const char* name = Theme_NameAt(s_themeCursor);
        char root[160];
        if (!name) return;
        Theme_RootFor(THEMES_DIR, name, root, sizeof(root));
        if (Theme_Load(root)) {
            XView_RefreshTheme();
            /* "default" stores empty so a missing custom theme always falls back */
            if (lstrcmpiA(name, "default") == 0) st->themeName[0] = '\0';
            else lstrcpynA(st->themeName, name, (int)sizeof(st->themeName));
            s_themeMsg = 1;
            Data_Save();
            Audio_PlaySfx(SFX_SELECT);
        }
        else {
            /* failed -> revert to default theme */
            Theme_Load("D:\\themes\\default");
            XView_RefreshTheme();
            st->themeName[0] = '\0';
            s_themeMsg = 2;
            Data_Save();
            Audio_PlaySfx(SFX_BACK);
        }
    }
}

static void UpdateFont(WORD pressed) {
    DD_Settings* st = Data_Get();
    if (pressed & BTN_DPAD_DOWN) { if (s_fontCursor < s_fontCount - 1) { s_fontCursor++; Audio_PlaySfx(SFX_NAV_DOWN); } }
    if (pressed & BTN_DPAD_UP) { if (s_fontCursor > 0) { s_fontCursor--; Audio_PlaySfx(SFX_NAV_UP); } }

    if (pressed & BTN_A) {
        if (s_fontCursor == 0) {
            /* Default = baked failsafe */
            Font_UseDefault(Gfx_Device());
            st->fontName[0] = '\0';
            s_fontMsg = 1;
            Data_Save();
            Audio_PlaySfx(SFX_SELECT);
        }
        else {
            char path[160];
            const char* name = s_fonts[s_fontCursor];
            lstrcpyA(path, FONT_DIR); lstrcatA(path, "\\");
            lstrcatA(path, name); lstrcatA(path, ".ddf");
            if (Font_LoadDDF(Gfx_Device(), path)) {
                lstrcpynA(st->fontName, name, (int)sizeof(st->fontName));
                s_fontMsg = 1;
                Data_Save();
                Audio_PlaySfx(SFX_SELECT);
            }
            else {
                /* load failed -> drop back to the failsafe */
                Font_UseDefault(Gfx_Device());
                st->fontName[0] = '\0';
                s_fontMsg = 2;
                Data_Save();
                Audio_PlaySfx(SFX_BACK);
            }
        }
    }
}

static void UpdateFtp(WORD pressed) {
    /* rows: 0 Service, 1 Port, 2 Username, 3 Password, 4 Apply */
    DD_Settings* st = Data_Get();
    if (pressed & BTN_DPAD_DOWN) { if (s_row < 4) { s_row++; Audio_PlaySfx(SFX_NAV_DOWN); } }
    if (pressed & BTN_DPAD_UP) { if (s_row > 0) { s_row--; Audio_PlaySfx(SFX_NAV_UP); } }

    if (s_row == 0 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT | BTN_A))) {
        st->ftpEnabled = !st->ftpEnabled;
        Audio_PlaySfx(SFX_ALT);
    }
    else if (s_row == 1 && (pressed & BTN_A)) {
        char cur[8]; IntToText(st->ftpPort, cur);
        s_ftpEdit = 1; Osk_Open(OSK_NUMERIC, cur, 5); Audio_PlaySfx(SFX_SELECT);
    }
    else if (s_row == 2 && (pressed & BTN_A)) {
        s_ftpEdit = 2; Osk_Open(OSK_TEXT, st->ftpUser, DD_FTP_CRED_MAX - 1); Audio_PlaySfx(SFX_SELECT);
    }
    else if (s_row == 3 && (pressed & BTN_A)) {
        s_ftpEdit = 3; Osk_Open(OSK_TEXT, st->ftpPass, DD_FTP_CRED_MAX - 1); Audio_PlaySfx(SFX_SELECT);
    }
    else if (s_row == 4 && (pressed & BTN_A)) {
        FtpApply(); Audio_PlaySfx(SFX_SELECT);
    }
}

static void UpdateNetwork(WORD pressed) {
    int kind;
    if (pressed & BTN_DPAD_DOWN) { if (s_row < NetRowCount() - 1) { s_row++; Audio_PlaySfx(SFX_NAV_DOWN); } }
    if (pressed & BTN_DPAD_UP) { if (s_row > 0) { s_row--; Audio_PlaySfx(SFX_NAV_UP); } }

    kind = NetRowKind(s_row);

    if (kind == 0 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
        if (pressed & BTN_DPAD_RIGHT) s_netMode = (s_netMode + 1) % 3;
        else                          s_netMode = (s_netMode + 2) % 3;
        if (s_row >= NetRowCount()) s_row = NetRowCount() - 1;
        s_netMsg = 0;
        Audio_PlaySfx(SFX_ALT);
    }
    else if (kind >= 1 && kind <= 5 && (pressed & BTN_A)) {
        char cur[20];
        FmtIp(*NetField(kind), cur);
        s_netEdit = kind;
        Osk_Open(OSK_NUMERIC, cur, 15);     /* numeric pad for the octets */
        Audio_PlaySfx(SFX_SELECT);
    }
    else if (kind == 6 && (pressed & BTN_A)) {
        int ok = Net_ApplyConfig(s_netMode, s_netIp, s_netMask, s_netGw, s_netDns1, s_netDns2);
        s_netMsg = ok ? 1 : 2;
        Audio_PlaySfx(ok ? SFX_SELECT : SFX_BACK);
    }
}

static void UpdateClock(WORD pressed) {
    /* rows: 0=Time 1=Month 2=Day 3=Year 4=NetTime 5=Zone 6=SyncNow 7=Apply */
    if (pressed & BTN_DPAD_DOWN) { if (s_row < 7) { s_row++; Audio_PlaySfx(SFX_NAV_DOWN); } }
    if (pressed & BTN_DPAD_UP) { if (s_row > 0) { s_row--; Audio_PlaySfx(SFX_NAV_UP); } }

    if (s_row == 0) {                                   /* Time */
        if (pressed & BTN_A) { s_clkPart ^= 1; Audio_PlaySfx(SFX_ALT); }   /* switch HH/MM */
        if (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT)) {
            int d = (pressed & BTN_DPAD_RIGHT) ? 1 : -1;
            if (s_clkPart == 0) s_clk.hour += d; else s_clk.min += d;
            ClampClock(); s_clkMsg = 0; Audio_PlaySfx(SFX_NAV_UP);
        }
    }
    else if (s_row >= 1 && s_row <= 3 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
        int d = (pressed & BTN_DPAD_RIGHT) ? 1 : -1;
        if (s_row == 1) s_clk.mon += d;
        else if (s_row == 2) s_clk.day += d;
        else s_clk.year += d;
        ClampClock(); s_clkMsg = 0; Audio_PlaySfx(SFX_NAV_UP);
    }
    else if (s_row == 4 && (pressed & (BTN_A | BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {  /* Net Time */
        Time_SetNtpEnabled(!Time_NtpEnabled());
        Time_Save();
        s_clkMsg = 0; Audio_PlaySfx(SFX_ALT);
    }
    else if (s_row == 5 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {          /* Zone */
        int idx = Time_TzIndex();
        idx += (pressed & BTN_DPAD_RIGHT) ? 1 : -1;
        Time_SetTzIndex(idx);
        Time_Save();
        s_clkMsg = 0; Audio_PlaySfx(SFX_NAV_UP);
    }
    else if (s_row == 6 && (pressed & BTN_A)) {          /* Sync Now */
        int ok = Ntp_Sync();
        s_clkMsg = ok ? 3 : 4;
        if (ok) Sys_GetClock(&s_clk);   /* refresh the editable fields from the new clock */
        Audio_PlaySfx(ok ? SFX_SELECT : SFX_BACK);
    }
    else if (s_row == 7 && (pressed & BTN_A)) {          /* Apply (manual) */
        s_clk.sec = 0;
        s_clkMsg = Sys_SetClock(&s_clk) ? 1 : 2;
        Audio_PlaySfx(s_clkMsg == 1 ? SFX_SELECT : SFX_BACK);
    }
}

/*---- visual control rows (RGB / OXFP) : shared declarations ----------------
   The RGB/OXFP screens build a row-descriptor list each frame so the Update and
   Render paths agree on what each row means even though the row set is mode
   -dependent. Declared here (above UpdateAccessories) so both can use it. */
enum {
    RR_MODE = 0, RR_BRIGHT, RR_SPEED, RR_INTENSITY, RR_PALCOUNT,
    RR_COLOR,            /* a color slot; .idx = 0..3 (RGB) or status/anim slot */
    RR_ANIMMODE, RR_ANIMSPEED, RR_ANIMCOLOR,
    RR_IDENTIFY, RR_SAVE, RR_RESET, RR_LABEL
};
typedef struct { int type; int idx; const char* label; } CtrlRow;

/* layout constants shared with DrawConsole's frame */
#define CR_MENUX   352.0f
#define CR_MENUY    48.0f
#define CR_ROWY0    96.0f
#define CR_LABELX  (CR_MENUX + 28.0f)
#define CR_WIDGETX (CR_MENUX + 150.0f)   /* where bars/swatches start            */
#define CR_WIDGETW   86.0f               /* bar / swatch width (virtual px)       */
/* Mode/Anim NAME values get their own column: the "Mode"/"Anim" labels are short,
   so the name can start left of the bars and run to the frame interior. This lets
   13-char names ("UNSC/Covenant", "Palette Cycle/Chase") fit in full. */
#define CR_NAMEVALX  (CR_MENUX + 112.0f) /* mode/anim name value start (left of bars) */
#define CR_VALRIGHT  (CR_MENUX + 252.0f) /* frame interior right limit (stay inside)  */

   /* Build the RGB row list for the current mode. Returns row count. */
static int BuildRgbRows(CtrlRow* r) {
    int n = 0, cc, i;
    r[n].type = RR_MODE;      r[n].label = "Mode";       r[n].idx = 0; n++;
    r[n].type = RR_BRIGHT;    r[n].label = "Brightness"; r[n].idx = 0; n++;
    r[n].type = RR_SPEED;     r[n].label = "Speed";      r[n].idx = 0; n++;
    r[n].type = RR_INTENSITY; r[n].label = "Intensity";  r[n].idx = 0; n++;
    cc = Rgb_ModeColorCount(s_rgbMode);
    if (cc < 0) {             /* palette mode: choose how many of A..D to use */
        r[n].type = RR_PALCOUNT; r[n].label = "Colors"; r[n].idx = 0; n++;
        cc = s_rgbPalCount;
    }
    for (i = 0; i < cc && i < 4; i++) {
        r[n].type = RR_COLOR; r[n].idx = i;
        r[n].label = (i == 0) ? "Color A" : (i == 1) ? "Color B"
            : (i == 2) ? "Color C" : "Color D";
        n++;
    }
    r[n].type = RR_SAVE;  r[n].label = "Save";  r[n].idx = 0; n++;
    r[n].type = RR_RESET; r[n].label = "Reset"; r[n].idx = 0; n++;
    return n;
}

/* Build the OXFP row list. */
static int BuildOxfpRows(CtrlRow* r) {
    int n = 0;
    r[n].type = RR_MODE;      r[n].label = "Mode";       r[n].idx = 0; n++;
    r[n].type = RR_BRIGHT;    r[n].label = "Brightness"; r[n].idx = 0; n++;
    r[n].type = RR_COLOR;     r[n].label = "Green";  r[n].idx = 0; n++;   /* status */
    r[n].type = RR_COLOR;     r[n].label = "Red";    r[n].idx = 1; n++;
    r[n].type = RR_COLOR;     r[n].label = "Orange"; r[n].idx = 2; n++;
    r[n].type = RR_ANIMMODE;  r[n].label = "Anim";       r[n].idx = 0; n++;
    r[n].type = RR_ANIMCOLOR; r[n].label = "Anim A"; r[n].idx = 0; n++;
    r[n].type = RR_ANIMCOLOR; r[n].label = "Anim B"; r[n].idx = 1; n++;
    r[n].type = RR_ANIMSPEED; r[n].label = "Anim Spd";   r[n].idx = 0; n++;
    r[n].type = RR_IDENTIFY;  r[n].label = "Identify";   r[n].idx = 0; n++;
    r[n].type = RR_SAVE;      r[n].label = "Save";       r[n].idx = 0; n++;
    r[n].type = RR_RESET;     r[n].label = "Reset";      r[n].idx = 0; n++;
    return n;
}

/* Adopt a device config snapshot into the working menu values + persist to
   dc.dat. Only fields the reply actually carried are applied (the rest keep
   their loaded dc.dat value), and ints are clamped to valid ranges. Colors
   snap to the nearest palette swatch. */
static void ApplyOxfpCfg(const OxfpDevCfg* c) {
    if (c->mode >= 0 && c->mode < OXFP_MODE_COUNT) s_oxfpMode = c->mode;
    if (c->brightness >= 0) { s_oxfpBright = c->brightness; if (s_oxfpBright > 255) s_oxfpBright = 255; }
    if (c->animMode >= 0 && c->animMode < OXFP_ANIM_COUNT) s_oxfpAnim = c->animMode;
    if (c->animSpeed >= 0) { s_oxfpAnimSpeed = c->animSpeed; if (s_oxfpAnimSpeed > 255) s_oxfpAnimSpeed = 255; }
    if (c->green[0] >= 0) s_oxfpStatusIx[0] = NearestPalette(c->green[0], c->green[1], c->green[2]);
    if (c->red[0] >= 0) s_oxfpStatusIx[1] = NearestPalette(c->red[0], c->red[1], c->red[2]);
    if (c->orange[0] >= 0) s_oxfpStatusIx[2] = NearestPalette(c->orange[0], c->orange[1], c->orange[2]);
    if (c->animA[0] >= 0) s_oxfpAnimIx[0] = NearestPalette(c->animA[0], c->animA[1], c->animA[2]);
    if (c->animB[0] >= 0) s_oxfpAnimIx[1] = NearestPalette(c->animB[0], c->animB[1], c->animB[2]);
    Dc_SaveOxfp2(s_oxfpMode, s_oxfpBright, s_oxfpAnim, s_oxfpAnimSpeed,
        s_oxfpStatusIx[0], s_oxfpStatusIx[1], s_oxfpStatusIx[2],
        s_oxfpAnimIx[0], s_oxfpAnimIx[1]);
}

static void ApplyRgbCfg(const RgbDevCfg* c) {
    if (c->mode >= 0 && c->mode < RGB_MODE_COUNT) s_rgbMode = c->mode;
    if (c->brightness >= 0) { s_rgbBright = c->brightness; if (s_rgbBright > 255) s_rgbBright = 255; }
    if (c->speed >= 0) { s_rgbSpeed = c->speed; if (s_rgbSpeed > 255) s_rgbSpeed = 255; }
    if (c->intensity >= 0) { s_rgbIntensity = c->intensity; if (s_rgbIntensity > 255) s_rgbIntensity = 255; }
    if (c->paletteCount >= 1) { s_rgbPalCount = c->paletteCount; if (s_rgbPalCount > 4) s_rgbPalCount = 4; }
    if (c->colorA >= 0) s_rgbColIx[0] = NearestPalette((int)((c->colorA >> 16) & 0xFF), (int)((c->colorA >> 8) & 0xFF), (int)(c->colorA & 0xFF));
    if (c->colorB >= 0) s_rgbColIx[1] = NearestPalette((int)((c->colorB >> 16) & 0xFF), (int)((c->colorB >> 8) & 0xFF), (int)(c->colorB & 0xFF));
    if (c->colorC >= 0) s_rgbColIx[2] = NearestPalette((int)((c->colorC >> 16) & 0xFF), (int)((c->colorC >> 8) & 0xFF), (int)(c->colorC & 0xFF));
    if (c->colorD >= 0) s_rgbColIx[3] = NearestPalette((int)((c->colorD >> 16) & 0xFF), (int)((c->colorD >> 8) & 0xFF), (int)(c->colorD & 0xFF));
    Dc_SaveRgb2(s_rgbMode, s_rgbBright, s_rgbSpeed, s_rgbIntensity, s_rgbPalCount,
        s_rgbColIx[0], s_rgbColIx[1], s_rgbColIx[2], s_rgbColIx[3]);
}

/*---- UpdateAccessories ------------------------------------------------------
   Two levels: a device list (LCD / Type-D), then each device's own screen.
   Device state/logic lives in dd_lcd / dd_typed; this is just the UI surface. */
static void UpdateAccessories(WORD pressed) {
    /* One-shot live-config pull: while a pull is in progress for the open RGB/
       OXFP screen, re-ask every 300ms and adopt the first valid reply that
       arrived after we entered. Times out at 2s; clears itself once applied so
       it never overwrites the user's live edits. */
    if (s_accSync && (s_accDev == ACC_DEV_RGB || s_accDev == ACC_DEV_OXFP)) {
        int  udev = (s_accDev == ACC_DEV_OXFP) ? UDP_DEV_OXFP : UDP_DEV_RGB;
        char buf[1600];
        unsigned long when = 0;
        DWORD now = GetTickCount();
        int n;
        if (!Udp_Present(udev) || (now - s_accSyncStart) > 2000UL) {
            s_accSync = 0;                       /* gone or timed out: keep dc.dat */
        }
        else {
            if (s_accSyncSent == 0 || (now - s_accSyncSent) > 300UL) {
                if (s_accDev == ACC_DEV_OXFP) Oxfp_RequestConfig();
                else Rgb_RequestConfig();
                s_accSyncSent = now;
            }
            n = Udp_LastReply(udev, buf, sizeof(buf), &when);
            if (n > 0 && when >= s_accSyncStart) {
                if (s_accDev == ACC_DEV_OXFP) {
                    OxfpDevCfg oc;
                    if (Oxfp_ParseConfig(buf, n, &oc)) { ApplyOxfpCfg(&oc); s_accSync = 0; }
                }
                else {
                    RgbDevCfg rc;
                    if (Rgb_ParseConfig(buf, n, &rc)) { ApplyRgbCfg(&rc); s_accSync = 0; }
                }
            }
        }
    }

    if (s_accDev < 0) {
        if (pressed & BTN_DPAD_DOWN) { if (s_accRow < ACC_DEV_COUNT - 1) { s_accRow++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_accRow > 0) { s_accRow--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & BTN_A) {
            /* Entry is always allowed. RGB/OXFP control items inside the screen
               are only selectable when the device is detected (handled in the
               screen itself), so you can always open the menu and SEE status. */
            if (s_accRow == ACC_DEV_RGB) {
                s_rgbMode = Dc_RgbMode(); s_rgbBright = Dc_RgbBright(); s_rgbSpeed = Dc_RgbSpeed();
                s_rgbIntensity = Dc_RgbIntensity(); s_rgbPalCount = Dc_RgbPalCount();
                s_rgbColIx[0] = Dc_RgbColor(0); s_rgbColIx[1] = Dc_RgbColor(1);
                s_rgbColIx[2] = Dc_RgbColor(2); s_rgbColIx[3] = Dc_RgbColor(3);
            }
            else if (s_accRow == ACC_DEV_OXFP) {
                s_oxfpMode = Dc_OxfpMode(); s_oxfpBright = Dc_OxfpBright();
                s_oxfpAnim = Dc_OxfpAnim(); s_oxfpAnimSpeed = Dc_OxfpAnimSpeed();
                /* clamp in case dc.dat holds a value from an older mode/anim list */
                if (s_oxfpMode < 0 || s_oxfpMode >= OXFP_MODE_COUNT) s_oxfpMode = 0;
                if (s_oxfpAnim < 0 || s_oxfpAnim >= OXFP_ANIM_COUNT) s_oxfpAnim = 0;
                s_oxfpStatusIx[0] = Dc_OxfpStatus(0); s_oxfpStatusIx[1] = Dc_OxfpStatus(1);
                s_oxfpStatusIx[2] = Dc_OxfpStatus(2);
                s_oxfpAnimIx[0] = Dc_OxfpAnimColor(0); s_oxfpAnimIx[1] = Dc_OxfpAnimColor(1);
            }
            s_accDev = s_accRow; s_accRow = 0; Audio_PlaySfx(SFX_SELECT);
            /* kick off a live-config pull so the screen shows the device's
               current state rather than dc.dat defaults (one-shot, see poll). */
            s_accSync = 0; s_accSyncSent = 0; s_accSyncStart = GetTickCount();
            if (s_accDev == ACC_DEV_OXFP && Oxfp_Present()) {
                s_accSync = 1; Oxfp_RequestConfig(); s_accSyncSent = GetTickCount();
            }
            else if (s_accDev == ACC_DEV_RGB && Rgb_Present()) {
                s_accSync = 1; Rgb_RequestConfig(); s_accSyncSent = GetTickCount();
            }
        }
        return;
    }

    if (s_accDev == ACC_DEV_XVIEW) {
        int nRows = 11;  /* Enabled, Interval, Brightness, Panel, + 7 page toggles */
        if (pressed & BTN_DPAD_DOWN) { if (s_accRow < nRows - 1) { s_accRow++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_accRow > 0) { s_accRow--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (s_accRow == 0 && (pressed & (BTN_A | BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
            XView_SetEnabled(!XView_IsEnabled()); Audio_PlaySfx(SFX_ALT);
        }
        else if (s_accRow == 1 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
            int iv = XView_IntervalMs();
            if (pressed & BTN_DPAD_RIGHT) iv += 1000;
            if (pressed & BTN_DPAD_LEFT)  iv -= 1000;
            XView_SetIntervalMs(iv); Audio_PlaySfx(SFX_ALT);
        }
        else if (s_accRow == 2 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
            int br = XView_Brightness();
            if (pressed & BTN_DPAD_RIGHT) br += 24;
            if (pressed & BTN_DPAD_LEFT)  br -= 24;
            XView_SetBrightness(br); Audio_PlaySfx(SFX_ALT);
        }
        else if (s_accRow == 3 && (pressed & (BTN_A | BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
            XView_SetPanel(XView_Panel() == XV_PANEL_B ? XV_PANEL_A : XV_PANEL_B);
            Audio_PlaySfx(SFX_ALT);
        }
        else if (s_accRow >= 4 && (pressed & BTN_A)) {
            static const int k_xvPageBit[7] = {
                LCD_PAGE_TEMPS, LCD_PAGE_MEM, LCD_PAGE_DISK,
                LCD_PAGE_NET, LCD_PAGE_FTP, LCD_PAGE_CLOCK, LCD_PAGE_NOWPLAYING
            };
            XView_TogglePage(k_xvPageBit[s_accRow - 4]); Audio_PlaySfx(SFX_ALT);
        }
        return;
    }

    if (s_accDev == ACC_DEV_LCD) {
        int nRows = 12;  /* Enabled, Address, Interval, Brightness, Compat, + 7 page toggles */
        if (pressed & BTN_DPAD_DOWN) { if (s_accRow < nRows - 1) { s_accRow++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_accRow > 0) { s_accRow--; Audio_PlaySfx(SFX_NAV_UP); } }

        if (s_accRow == 0 && (pressed & BTN_A)) {
            Lcd_SetEnabled(!Lcd_Enabled()); Audio_PlaySfx(SFX_ALT);
        }
        else if (s_accRow == 1 && (pressed & (BTN_A | BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
            Lcd_SetAddrChoice(Lcd_AddrChoice() == LCD_ADDR_3C ? LCD_ADDR_3D : LCD_ADDR_3C);
            Audio_PlaySfx(SFX_ALT);
        }
        else if (s_accRow == 2 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
            int iv = Lcd_IntervalMs();
            if (pressed & BTN_DPAD_RIGHT) iv += 1000;
            if (pressed & BTN_DPAD_LEFT)  iv -= 1000;
            Lcd_SetIntervalMs(iv); Audio_PlaySfx(SFX_ALT);
        }
        else if (s_accRow == 3 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
            int br = Lcd_Brightness();
            if (pressed & BTN_DPAD_RIGHT) br += 24;
            if (pressed & BTN_DPAD_LEFT)  br -= 24;
            Lcd_SetBrightness(br); Audio_PlaySfx(SFX_ALT);
        }
        else if (s_accRow == 4 && (pressed & (BTN_A | BTN_DPAD_LEFT | BTN_DPAD_RIGHT))) {
            Lcd_SetCompatMode(!Lcd_CompatMode()); Audio_PlaySfx(SFX_ALT);
        }
        else if (s_accRow >= 5 && (pressed & BTN_A)) {
            static const int k_pageBit[7] = {
                LCD_PAGE_TEMPS, LCD_PAGE_MEM, LCD_PAGE_DISK,
                LCD_PAGE_NET, LCD_PAGE_FTP, LCD_PAGE_CLOCK, LCD_PAGE_NOWPLAYING
            };
            Lcd_TogglePage(k_pageBit[s_accRow - 5]); Audio_PlaySfx(SFX_ALT);
        }
        return;
    }

    if (s_accDev == ACC_DEV_TYPED) {
        /* gate the art rows on their device class being detected on the network:
           a non-present row is unselectable (nav skips it, A is inert) so we
           never enable art -- or fire a send -- for a device that isn't there. */
        int xlPresent = Udp_TypeDPresent(5);
        int ctrlPresent = (Udp_TypeDPresent(1) || Udp_TypeDPresent(2) ||
            Udp_TypeDPresent(3) || Udp_TypeDPresent(4));
        int anyPresent = (xlPresent || ctrlPresent);
        int sel[4];
        sel[0] = 1;             /* Enabled: always selectable          */
        sel[1] = xlPresent;     /* XL Art: only if the XL is present    */
        sel[2] = ctrlPresent;   /* Type-D Art: only if a 1-4 is present */
        sel[3] = anyPresent;    /* Resume: only if any unit is present  */

        /* if the highlight is parked on a now-unselectable row, snap home */
        if (s_accRow < 0 || s_accRow > 3 || !sel[s_accRow]) s_accRow = 0;

        if (pressed & BTN_DPAD_DOWN) {
            int r = s_accRow;
            while (r < 3) { r++; if (sel[r]) { s_accRow = r; Audio_PlaySfx(SFX_NAV_DOWN); break; } }
        }
        if (pressed & BTN_DPAD_UP) {
            int r = s_accRow;
            while (r > 0) { r--; if (sel[r]) { s_accRow = r; Audio_PlaySfx(SFX_NAV_UP); break; } }
        }
        if (pressed & BTN_A) {
            if (s_accRow == 0) { TypeD_SetEnabled(!TypeD_Enabled()); Audio_PlaySfx(SFX_ALT); }
            else if (s_accRow == 1 && sel[1]) { Dc_SetTypeDArtEnabled(!Dc_TypeDArtEnabled()); Audio_PlaySfx(SFX_ALT); }           /* XL Art */
            else if (s_accRow == 2 && sel[2]) { Dc_SetTypeDCtrlArtEnabled(!Dc_TypeDCtrlArtEnabled()); Audio_PlaySfx(SFX_ALT); }   /* Type-D Art (1-4) */
            else if (s_accRow == 3 && sel[3]) { TypeDArt_Resume(); Audio_PlaySfx(SFX_SELECT); }
        }
        return;
    }

    /* ----- XBOX-RGB screen (row-descriptor driven; matches the renderer) ----- */
    if (s_accDev == ACC_DEV_RGB) {
        CtrlRow rows[14]; int n = BuildRgbRows(rows);
        int present = Udp_Present(UDP_DEV_RGB);
        int dir = (pressed & BTN_DPAD_RIGHT) ? 1 : (pressed & BTN_DPAD_LEFT) ? -1 : 0;
        if (s_accRow >= n) s_accRow = n - 1;
        if (pressed & BTN_DPAD_DOWN) { if (s_accRow < n - 1) { s_accRow++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_accRow > 0) { s_accRow--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (!present) {                          /* inert when not detected */
            if (pressed & (BTN_A | BTN_DPAD_LEFT | BTN_DPAD_RIGHT)) Audio_PlaySfx(SFX_BACK);
            return;
        }
        {
            const CtrlRow* rr = &rows[s_accRow];
            switch (rr->type) {
            case RR_MODE:
                if (dir) {
                    s_rgbMode = (s_rgbMode + RGB_MODE_COUNT + dir) % RGB_MODE_COUNT;
                    Rgb_SetMode(s_rgbMode, 0); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_BRIGHT:
                if (dir) {
                    s_rgbBright += dir * 16; if (s_rgbBright < 0) s_rgbBright = 0; if (s_rgbBright > 255) s_rgbBright = 255;
                    Rgb_SetBrightness(s_rgbBright, 0); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_SPEED:
                if (dir) {
                    s_rgbSpeed += dir * 16; if (s_rgbSpeed < 0) s_rgbSpeed = 0; if (s_rgbSpeed > 255) s_rgbSpeed = 255;
                    Rgb_SetSpeed(s_rgbSpeed, 0); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_INTENSITY:
                if (dir) {
                    s_rgbIntensity += dir * 16; if (s_rgbIntensity < 0) s_rgbIntensity = 0; if (s_rgbIntensity > 255) s_rgbIntensity = 255;
                    Rgb_SetIntensity(s_rgbIntensity, 0); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_PALCOUNT:
                if (dir) {
                    s_rgbPalCount += dir; if (s_rgbPalCount < 1) s_rgbPalCount = 1; if (s_rgbPalCount > 4) s_rgbPalCount = 4;
                    Rgb_SetPaletteCount(s_rgbPalCount, 0); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_COLOR:
                if (dir) {
                    int* ix = &s_rgbColIx[rr->idx];
                    *ix = (*ix + PALETTE_COUNT + dir) % PALETTE_COUNT;
                    Rgb_SetColor(rr->idx, k_palette[*ix], 0); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_SAVE:
                if (pressed & BTN_A) {
                    int i, cc;
                    Rgb_SetMode(s_rgbMode, 1); Rgb_SetBrightness(s_rgbBright, 1);
                    Rgb_SetSpeed(s_rgbSpeed, 1); Rgb_SetIntensity(s_rgbIntensity, 1);
                    cc = Rgb_ModeColorCount(s_rgbMode);
                    if (cc < 0) { Rgb_SetPaletteCount(s_rgbPalCount, 1); cc = s_rgbPalCount; }
                    for (i = 0; i < cc && i < 4; i++) Rgb_SetColor(i, k_palette[s_rgbColIx[i]], 1);
                    Dc_SaveRgb2(s_rgbMode, s_rgbBright, s_rgbSpeed, s_rgbIntensity,
                        s_rgbPalCount, s_rgbColIx[0], s_rgbColIx[1], s_rgbColIx[2], s_rgbColIx[3]);
                    Audio_PlaySfx(SFX_SELECT);
                }
                break;
            case RR_RESET:
                if (pressed & BTN_A) { Rgb_Reset(); Audio_PlaySfx(SFX_ALT); }
                break;
            default: break;
            }
        }
        return;
    }

    /* ----- OXFP screen (row-descriptor driven) ----- */
    if (s_accDev == ACC_DEV_OXFP) {
        CtrlRow rows[14]; int n = BuildOxfpRows(rows);
        int present = Udp_Present(UDP_DEV_OXFP);
        int dir = (pressed & BTN_DPAD_RIGHT) ? 1 : (pressed & BTN_DPAD_LEFT) ? -1 : 0;
        if (s_accRow >= n) s_accRow = n - 1;
        if (pressed & BTN_DPAD_DOWN) { if (s_accRow < n - 1) { s_accRow++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_accRow > 0) { s_accRow--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (!present) {
            if (pressed & (BTN_A | BTN_DPAD_LEFT | BTN_DPAD_RIGHT)) Audio_PlaySfx(SFX_BACK);
            return;
        }
        {
            const CtrlRow* rr = &rows[s_accRow];
            switch (rr->type) {
            case RR_MODE:
                if (dir) {
                    s_oxfpMode = (s_oxfpMode + OXFP_MODE_COUNT + dir) % OXFP_MODE_COUNT;
                    Oxfp_SetMode(s_oxfpMode); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_BRIGHT:
                if (dir) {
                    s_oxfpBright += dir * 16; if (s_oxfpBright < 0) s_oxfpBright = 0; if (s_oxfpBright > 255) s_oxfpBright = 255;
                    Oxfp_SetBrightness(s_oxfpBright); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_COLOR:   /* status color: green/red/orange */
                if (dir) {
                    int* ix = &s_oxfpStatusIx[rr->idx];
                    *ix = (*ix + PALETTE_COUNT + dir) % PALETTE_COUNT;
                    Oxfp_SetStatusColor(rr->idx, k_palette[*ix]); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_ANIMMODE:
                if (dir) {
                    s_oxfpAnim = (s_oxfpAnim + OXFP_ANIM_COUNT + dir) % OXFP_ANIM_COUNT;
                    Oxfp_SetAnimMode(s_oxfpAnim); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_ANIMCOLOR:
                if (dir) {
                    int* ix = &s_oxfpAnimIx[rr->idx];
                    *ix = (*ix + PALETTE_COUNT + dir) % PALETTE_COUNT;
                    Oxfp_SetAnimColor(rr->idx, k_palette[*ix]); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_ANIMSPEED:
                if (dir) {
                    s_oxfpAnimSpeed += dir * 16; if (s_oxfpAnimSpeed < 0) s_oxfpAnimSpeed = 0; if (s_oxfpAnimSpeed > 255) s_oxfpAnimSpeed = 255;
                    Oxfp_SetAnimSpeed(s_oxfpAnimSpeed); Audio_PlaySfx(SFX_ALT);
                }
                break;
            case RR_IDENTIFY:
                if (pressed & BTN_A) { Oxfp_Identify(1500); Audio_PlaySfx(SFX_ALT); }
                break;
            case RR_SAVE:
                if (pressed & BTN_A) {
                    Oxfp_Save();
                    Dc_SaveOxfp2(s_oxfpMode, s_oxfpBright, s_oxfpAnim, s_oxfpAnimSpeed,
                        s_oxfpStatusIx[0], s_oxfpStatusIx[1], s_oxfpStatusIx[2],
                        s_oxfpAnimIx[0], s_oxfpAnimIx[1]);
                    Audio_PlaySfx(SFX_SELECT);
                }
                break;
            case RR_RESET:
                if (pressed & BTN_A) { Oxfp_Reset(); Audio_PlaySfx(SFX_ALT); }
                break;
            default: break;
            }
        }
        return;
    }
}

static void UpdateVideo(WORD pressed) {
    DD_Settings* st = Data_Get();

    /* ---- Effects sub-menu: toggle the four effect flags ---- */
    if (s_fxSub) {
        if (pressed & BTN_DPAD_DOWN) { if (s_fxRow < 5) { s_fxRow++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_fxRow > 0) { s_fxRow--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT | BTN_A)) {
            st->fxFlags ^= k_fxBits[s_fxRow];      /* flip the bit (all-off is valid + persists) */
            Data_Save();
            Audio_PlaySfx(SFX_ALT);
        }
        if (pressed & BTN_B) { s_fxSub = 0; Select_Reset(); Audio_PlaySfx(SFX_BACK); }
        return;
    }

    if (pressed & BTN_DPAD_DOWN) { if (s_row < 4) { s_row++; Audio_PlaySfx(SFX_NAV_DOWN); } }
    if (pressed & BTN_DPAD_UP) { if (s_row > 0) { s_row--; Audio_PlaySfx(SFX_NAV_UP); } }
    if (s_row == 0 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT | BTN_A))) {
        st->videoAspect = (st->videoAspect == DD_VIDEO_STRETCH)
            ? DD_VIDEO_PILLARBOX : DD_VIDEO_STRETCH;
        UI_SetStretch(st->videoAspect == DD_VIDEO_STRETCH);   /* live */
        Calib_Apply();   /* re-fold calibration after the stretch mode changed */
        Audio_PlaySfx(SFX_ALT);
    }
    if (s_row == 1 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT | BTN_A))) {
        /* cycle Auto -> 480p -> 720p; applied at next boot (device re-init) */
        if (pressed & BTN_DPAD_LEFT) st->videoRes = (st->videoRes + 2) % 3;
        else                         st->videoRes = (st->videoRes + 1) % 3;
        Audio_PlaySfx(SFX_ALT);
    }
    if (s_row == 2 && (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT | BTN_A))) {
        /* screensaver timeout: Off, 5, 10, 15, 20, 30 min */
        static const int k_ss[6] = { 0, 5, 10, 15, 20, 30 };
        int cur = 0, i;
        for (i = 0; i < 6; i++) if (st->screensaverMin == k_ss[i]) { cur = i; break; }
        if (pressed & BTN_DPAD_LEFT) cur = (cur + 5) % 6;
        else                         cur = (cur + 1) % 6;
        st->screensaverMin = k_ss[cur];
        Audio_PlaySfx(SFX_ALT);
    }
    if (s_row == 3 && (pressed & BTN_A)) {
        Audio_PlaySfx(SFX_SELECT);
        Calib_Run();     /* interactive overscan overlay; saves + applies live */
    }
    if (s_row == 4 && (pressed & BTN_A)) {
        s_fxSub = 1; s_fxRow = 0; Select_Reset(); Audio_PlaySfx(SFX_SELECT);   /* open Effects */
    }
}

int Settings_Update(WORD pressed, WORD held) {
    (void)held;
    if (s_view < 0) return UpdateList(pressed);

    /* OSK overlay (numeric IP entry) takes all input while open */
    if (Osk_IsOpen()) {
        int r = Osk_Update(pressed);
        if (r != 0) {
            if (r == 1 && s_netEdit >= 1 && s_netEdit <= 5) {
                char buf[24];
                Osk_GetText(buf, sizeof(buf));
                *NetField(s_netEdit) = ParseIp(buf);
            }
            else if (r == 1 && s_ftpEdit) {
                char buf[40];
                DD_Settings* st = Data_Get();
                Osk_GetText(buf, sizeof(buf));
                if (s_ftpEdit == 1) {
                    int p = 0, i = 0;
                    while (buf[i] >= '0' && buf[i] <= '9') { p = p * 10 + (buf[i] - '0'); i++; }
                    if (p < 1 || p > 65535) p = 21;
                    st->ftpPort = p;
                }
                else if (s_ftpEdit == 2) {
                    if (buf[0]) lstrcpynA(st->ftpUser, buf, DD_FTP_CRED_MAX);
                }
                else if (s_ftpEdit == 3) {
                    if (buf[0]) lstrcpynA(st->ftpPass, buf, DD_FTP_CRED_MAX);
                }
            }
            s_netEdit = -1; s_ftpEdit = 0;
        }
        return 0;
    }

    if (s_view == CAT_AUDIO) UpdateAudio(pressed);
    else if (s_view == CAT_FAN) UpdateFan(pressed);
    else if (s_view == CAT_VIDEO) {
        /* if the Effects sub-menu is open, it owns B (close sub, not Video) */
        int subWasOpen = s_fxSub;
        UpdateVideo(pressed);
        if (subWasOpen) return 0;   /* swallow this frame's input incl. B */
    }
    else if (s_view == CAT_CLOCK) UpdateClock(pressed);
    else if (s_view == CAT_NETWORK) UpdateNetwork(pressed);
    else if (s_view == CAT_FTP) UpdateFtp(pressed);
    else if (s_view == CAT_FONT) UpdateFont(pressed);
    else if (s_view == CAT_THEME) UpdateTheme(pressed);
    else if (s_view == CAT_UPDATE) UpdateUpdate(pressed);
    else if (s_view == CAT_ACCESSORIES) {
        /* two-level: B inside a device screen returns to the device list, not
           out to the category list -- so swallow B at that level */
        UpdateAccessories(pressed);
        if (s_accDev >= 0 && (pressed & BTN_B)) {
            s_accDev = -1; s_accRow = 0; s_accSync = 0; Audio_PlaySfx(SFX_BACK);
            return 0;
        }
    }

    if (pressed & BTN_B) {
        if (s_view == CAT_AUDIO && s_audioPick) { Audio_PlaySfx(SFX_BACK); s_audioPick = 0; return 0; }
        Audio_PlaySfx(SFX_BACK);
        if (s_view == CAT_AUDIO) Data_Save();
        if (s_view == CAT_VIDEO) Data_Save();
        if (s_view == CAT_FTP) Data_Save();
        if (s_view == CAT_FAN) {
            DD_Settings* st = Data_Get();
            st->fanAuto = s_fanAuto; st->fanPercent = s_fanPct; Data_Save();
        }
        if (s_view == CAT_UPDATE) Upd_Cancel();   /* abort any in-flight check */
        if (s_view == CAT_ACCESSORIES) { s_accDev = -1; s_accRow = 0; s_accSync = 0; }
        s_view = -1; s_row = 0;
        Select_Reset();   /* snap back to the category list position */
    }
    return 0;
}

/*---- render: list ----------------------------------------------------------*/

static void RenderList(IDirect3DDevice8* d) {
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* menu = Theme_Asset("frame_menu_v");
    float menuX = 352.0f, menuY = 48.0f, rowY0 = 80.0f, rowDY = 34.0f;
    int i;
    /* visible-row window: the panel interior fits ~9 rows; scroll to keep the
       cursor in view once the list outgrows the frame (Accessories pushed it
       to 11). */
    int visRows = 9;
    static int s_listScroll = 0;
    if (s_cursor < s_listScroll) s_listScroll = s_cursor;
    if (s_cursor >= s_listScroll + visRows) s_listScroll = s_cursor - visRows + 1;
    if (s_listScroll < 0) s_listScroll = 0;
    if (s_listScroll > SET_COUNT - visRows && SET_COUNT > visRows) s_listScroll = SET_COUNT - visRows;
    if (SET_COUNT <= visRows) s_listScroll = 0;

    Chrome(d, "SETTINGS", "A OPEN   B BACK");
    DrawPedestal(d);

    Iso_Begin();
    if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
    {
        float gy = rowY0 + rowDY * (float)(s_cursor - s_listScroll) - 6.0f;
        Select_Begin(0x5001, gy);
        Select_DrawGlow(menuX + 18.0f, gy, 210.0f, 32.0f, UI_ARGB(110, ar, ag, ab));
    }
    for (i = 0; i < visRows; i++) {
        int idx = s_listScroll + i;
        DWORD c;
        if (idx >= SET_COUNT) break;
        c = (idx == s_cursor) ? glow : text;
        Font_DrawTextIso(d, menuX + 30.0f, rowY0 + rowDY * (float)i + 4.0f, k_items[idx], FONT_SIZE_MEDIUM, c);
    }
    Iso_End();
}

/*---- render: About (framed auto-scroller, intentionally distinct) ----------*/

static void RenderAbout(IDirect3DDevice8* d) {
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    const Texture* frame = Theme_Asset("frame_menu_v");
    int   vx = 90, vy = 70, vw = 460, vh = 330;
    float scrollPx, contentH;
    int   nLines = 0, li;
    char  lines[24][72];
    char  num[16], freeStr[40];
    D3DVIEWPORT8 vpOld, vpClip;

    Chrome(d, "ABOUT", "B BACK");
    if (frame) UI_DrawSprite(frame, (float)(vx - 18), (float)(vy - 14),
        (float)(vw + 36), (float)(vh + 28), 0xFFFFFFFF, 0);

    strcpy(lines[nLines++], "DarkDash");
    lines[nLines][0] = 0; strcat(lines[nLines], "Version   "); strcat(lines[nLines++], DARKDASH_VERSION);
    lines[nLines][0] = 0; strcat(lines[nLines], "Build     "); strcat(lines[nLines++], DARKDASH_BUILD);
    strcpy(lines[nLines++], "Author    Darkone83");
    strcpy(lines[nLines++], "");
    lines[nLines][0] = 0; strcat(lines[nLines], "Console   "); strcat(lines[nLines++], Sys_XboxRevision());
    {
        lines[nLines][0] = 0; strcat(lines[nLines], "CPU       Pentium III  ");
        IntToText((int)Sys_CpuMHz(), num); strcat(lines[nLines], num); strcat(lines[nLines++], " MHz");
    }
    {
        lines[nLines][0] = 0; strcat(lines[nLines], "GPU       NV2A  ");
        IntToText((int)Sys_GpuMHz(), num); strcat(lines[nLines], num); strcat(lines[nLines++], " MHz");
    }
    {
        int mb = Sys_RamMB(), fr = Sys_RamFreeMB();
        lines[nLines][0] = 0; strcat(lines[nLines], "RAM       ");
        IntToText(mb, num); strcat(lines[nLines], num); strcat(lines[nLines], " MB total, ");
        IntToText(fr, num); strcat(lines[nLines], num); strcat(lines[nLines++], " MB free");
    }
    {
        lines[nLines][0] = 0; strcat(lines[nLines], "Video     ");
        IntToText(Gfx_Width(), num); strcat(lines[nLines], num); strcat(lines[nLines], "x");
        IntToText(Gfx_Height(), num); strcat(lines[nLines], num); strcat(lines[nLines], " ");
        strcat(lines[nLines++], Gfx_VideoModeStr());
    }
    /* live health: CPU / board temp + fan % */
    {
        int cpuC = 0, brdC = 0, fan = 0;
        if (Sys_ReadTemps(&cpuC, &brdC)) {
            lines[nLines][0] = 0; strcat(lines[nLines], "CPU Temp  ");
            IntToText(cpuC, num); strcat(lines[nLines], num); strcat(lines[nLines++], " C");
            lines[nLines][0] = 0; strcat(lines[nLines], "Board     ");
            IntToText(brdC, num); strcat(lines[nLines], num); strcat(lines[nLines++], " C");
        }
        if (Sys_ReadFanPct(&fan)) {
            lines[nLines][0] = 0; strcat(lines[nLines], "Fan       ");
            IntToText(fan, num); strcat(lines[nLines], num); strcat(lines[nLines++], " %");
        }
    }
    strcpy(lines[nLines++], "");
    /* partitions: free / total. Only list drives that are actually mounted. */
    {
        static const char* const k_part[] = { "C:\\", "E:\\", "F:\\", "G:\\", "X:\\", "Y:\\", "Z:\\", 0 };
        int pi;
        for (pi = 0; k_part[pi]; pi++) {
            Sys_DiskUsageStr(k_part[pi], freeStr, sizeof(freeStr));
            if (freeStr[0] && freeStr[0] != '-') {
                char* p = lines[nLines];
                p[0] = k_part[pi][0]; p[1] = ':'; p[2] = ' '; p[3] = ' '; p[4] = ' '; p[5] = ' ';
                p[6] = ' '; p[7] = ' '; p[8] = ' '; p[9] = ' '; p[10] = 0;
                strcat(lines[nLines++], freeStr);
            }
        }
    }
    lines[nLines][0] = 0; strcat(lines[nLines], "IP        "); strcat(lines[nLines++], Net_Ip());

    contentH = (float)nLines * 40.0f + (float)vh;
    scrollPx = (float)(((GetTickCount() - s_aboutEnter) / 24) % (DWORD)contentH);

    d->GetViewport(&vpOld);
    vpClip.X = (DWORD)UI_Sx((float)vx); vpClip.Y = (DWORD)UI_Sy((float)vy);
    vpClip.Width = (DWORD)UI_ScaleX((float)vw); vpClip.Height = (DWORD)UI_ScaleY((float)vh);
    vpClip.MinZ = 0.0f; vpClip.MaxZ = 1.0f;
    d->SetViewport(&vpClip);
    for (li = 0; li < nLines; li++) {
        float ly = (float)vy + (float)vh + (float)li * 40.0f - scrollPx;
        if (lines[li][0] == 0) continue;
        Font_DrawText(d, (float)(vx + 24), ly, lines[li],
            (li == 0) ? FONT_SIZE_LARGE : FONT_SIZE_MEDIUM,
            (li == 0) ? accent : text, 0);
    }
    d->SetViewport(&vpOld);
}

/*---- render: Audio (launcher-style) ----------------------------------------*/

static void RenderAudio(IDirect3DDevice8* d) {
    DD_Settings* st = Data_Get();
    char volRow[48], trkRow[64], outRow[40], ac3Row[40], dtsRow[40], num[8];
    const char* rows[5];
    int  k;
    int  amode = 0, aac3 = 0, adts = 0;

    if (s_audioPick) {
        DWORD text = Theme_Color("text", 0xFFD8F8C0);
        DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
        DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
        DWORD accent = Theme_Color("accent", 0xFF7FE000);
        int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
        const Texture* menu = Theme_Asset("frame_menu_v");
        float menuX = 352.0f, menuY = 48.0f, rowY0 = 90.0f, rowDY = 34.0f;
        int pickCount = 2 + s_mp3Count;              /* None + Shuffle + files */
        int i, vis = (pickCount < 9) ? pickCount : 9;
        Chrome(d, "SELECT MUSIC", "A USE   X BUILT-IN   B BACK");
        DrawPedestal(d);
        Iso_Begin();
        if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
        {
            float gy = rowY0 + rowDY * (float)s_mp3Cursor - 6.0f;
            Iso_FillRect(menuX + 16.0f, gy, 214.0f, 30.0f, UI_ARGB(110, ar, ag, ab), 1);
            for (i = 0; i < vis; i++) {
                DWORD c = (i == s_mp3Cursor) ? glow : text;
                const char* label = (i == 0) ? "None" : (i == 1) ? "Shuffle" : s_mp3[i - 2];
                Font_DrawTextIso(d, menuX + 26.0f, rowY0 + rowDY * (float)i + 2.0f, label, FONT_SIZE_SMALL, c);
            }
            if (s_mp3Count == 0)
                Font_DrawTextIso(d, menuX + 26.0f, rowY0 + rowDY * (float)vis + 8.0f,
                    "(no .mp3 files)", FONT_SIZE_SMALL, dim);
        }
        Iso_End();
        return;
    }

    k = IntToText(st->musicVolume, num); num[k] = '%'; num[k + 1] = 0;
    strcpy(volRow, "Volume    "); strcat(volRow, num);
    strcpy(trkRow, "Track     ");
    if (st->musicMode == DD_MUSIC_NONE) {
        strcat(trkRow, "None");
    }
    else if (st->musicMode == DD_MUSIC_SHUFFLE) {
        strcat(trkRow, "Shuffle");
    }
    else if (st->musicCustom && st->musicPath[0]) {
        const char* p = st->musicPath; const char* base = p;
        int tl = 0;
        while (*p) { if (*p == '\\') base = p + 1; p++; }
        /* bounded append: a long filename (musicPath can be far longer than this
           row) must never run past trkRow -- that was a stack stomp into volRow. */
        while (trkRow[tl]) tl++;
        while (*base && tl < (int)sizeof(trkRow) - 1) trkRow[tl++] = *base++;
        trkRow[tl] = 0;
    }
    else {
        strcat(trkRow, "Built-in");
    }
    rows[0] = volRow; rows[1] = trkRow;

    /* console audio config (EEPROM-backed) */
    Eeprom_GetAudio(&amode, &aac3, &adts);
    strcpy(outRow, "Output    ");
    strcat(outRow, (amode == DD_AUDIO_MONO) ? "Mono"
        : (amode == DD_AUDIO_SURROUND) ? "Surround" : "Stereo");
    strcpy(ac3Row, "Dolby AC3 "); strcat(ac3Row, aac3 ? "On" : "Off");
    strcpy(dtsRow, "DTS       "); strcat(dtsRow, adts ? "On" : "Off");
    rows[2] = outRow; rows[3] = ac3Row; rows[4] = dtsRow;

    Chrome(d, "AUDIO", "L/R ADJUST   A PICK   B BACK");
    DrawPedestal(d);
    DrawConsole(d, rows, 5, s_row, 5, 0);
    if (s_audioMsg) {
        DWORD ok = Theme_Color("accent", 0xFF7FE000);
        DWORD dm = Theme_Color("text_dim", 0xFF7FA060);
        Font_DrawText(d, 60.0f, 410.0f,
            (s_audioMsg == 1) ? "Saved -- restart games to apply" : "Save failed",
            FONT_SIZE_SMALL, (s_audioMsg == 1) ? ok : dm, 0);
    }
}

/*---- render: Fan (launcher-style) ------------------------------------------*/

static void RenderFan(IDirect3DDevice8* d) {
    char modeRow[40], spdRow[40], curRow[40], num[8];
    const char* rows[3];
    int  nRows, selectable, curFan = 0, k;

    strcpy(modeRow, "Mode      "); strcat(modeRow, s_fanAuto ? "Automatic" : "Manual");

    if (s_fanAuto) {
        if (Sys_ReadFanPct(&curFan)) { strcpy(curRow, "Current   "); k = IntToText(curFan, num); strcat(curRow, num); strcat(curRow, "%"); }
        else { strcpy(curRow, "Current   --"); }
        rows[0] = modeRow; rows[1] = curRow;
        nRows = 2; selectable = 1;
    }
    else {
        k = IntToText(s_fanPct, num); num[k] = '%'; num[k + 1] = 0;
        strcpy(spdRow, "Speed     "); strcat(spdRow, num);
        if (Sys_ReadFanPct(&curFan)) { strcpy(curRow, "Current   "); k = IntToText(curFan, num); strcat(curRow, num); strcat(curRow, "%"); }
        else { strcpy(curRow, "Current   --"); }
        rows[0] = modeRow; rows[1] = spdRow; rows[2] = curRow;
        nRows = 3; selectable = 2;
    }

    Chrome(d, "FAN", "L/R ADJUST   B BACK");
    DrawPedestal(d);
    DrawConsole(d, rows, nRows, s_row, selectable, 0);
}

/*---- render: defensive fallback. Every category has a real renderer (see the
       dispatch in Settings_Render); this is only reached if s_view is ever out
       of range, and simply shows "Coming soon" rather than a blank screen. ---*/

static void RenderPlaceholder(IDirect3DDevice8* d, const char* name) {
    const char* rows[1];
    rows[0] = "Coming soon";
    Chrome(d, name, "B BACK");
    DrawPedestal(d);
    DrawConsole(d, rows, 1, -1, 0, 0);
}

/*---- render: Video (launcher-style) ----------------------------------------*/

static void RenderVideo(IDirect3DDevice8* d) {
    DD_Settings* st = Data_Get();
    char aspRow[40], resRow[40], curRow[40], ssRow[40], num[8];
    const char* rows[6];

    /* ---- Effects sub-menu: four On/Off toggles ---- */
    if (s_fxSub) {
        char fxRows[6][40];
        const char* rp[6];
        int i;
        for (i = 0; i < 6; i++) {
            int onoff = (st->fxFlags & k_fxBits[i]) ? 1 : 0;
            strcpy(fxRows[i], k_fxNames[i]);
            /* pad to a column, then On/Off */
            while ((int)strlen(fxRows[i]) < 14) strcat(fxRows[i], " ");
            strcat(fxRows[i], onoff ? "On" : "Off");
            rp[i] = fxRows[i];
        }
        Chrome(d, "EFFECTS", "A TOGGLE   B BACK");
        DrawPedestal(d);
        DrawConsole(d, rp, 6, s_fxRow, 6, 0);
        {
            DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
            Font_DrawText(d, 60.0f, 410.0f, "Customize the dashboard effects",
                FONT_SIZE_SMALL, dim, 0);
        }
        return;
    }

    strcpy(aspRow, "Aspect    ");
    strcat(aspRow, (st->videoAspect == DD_VIDEO_STRETCH) ? "Stretch" : "Pillarbox");

    strcpy(resRow, "Resolution ");
    if (st->videoRes == DD_RES_480)      strcat(resRow, "480p");
    else if (st->videoRes == DD_RES_720) strcat(resRow, "720p");
    else                                 strcat(resRow, "Auto");

    strcpy(ssRow, "Screensaver ");
    if (st->screensaverMin <= 0) {
        strcat(ssRow, "Off");
    }
    else {
        IntToText(st->screensaverMin, num); strcat(ssRow, num); strcat(ssRow, " min");
    }

    /* live readout: the video STANDARD we booted, not the raw framebuffer size.
       Gfx_VideoModeStr() reports what DarkDash actually output (480i/480p/576i/
       720p); append the console's region (NTSC/NTSC-J/PAL) for context so the
       user sees e.g. "480i NTSC". 1080i is shown as a capability note when the
       display reports it (the dash itself renders at 480/720). */
    strcpy(curRow, "Output    ");
    strcat(curRow, Gfx_VideoModeStr());
    {
        DWORD vstd = XGetVideoStandard();
        const char* reg =
            (vstd == XC_VIDEO_STANDARD_NTSC_J) ? " NTSC-J" :
            (vstd == XC_VIDEO_STANDARD_PAL_I) ? " PAL" :
            (vstd == XC_VIDEO_STANDARD_NTSC_M) ? " NTSC" : "";
        strcat(curRow, reg);
    }

    rows[0] = aspRow; rows[1] = resRow; rows[2] = ssRow;
    rows[3] = "Calibrate Screen"; rows[4] = "Effects"; rows[5] = curRow;

    Chrome(d, "VIDEO", "L/R CHANGE   A SELECT   B BACK");
    DrawPedestal(d);
    DrawConsole(d, rows, 6, s_row, 5, 0);   /* rows 0-4 selectable; Output (5) read-only */

    /* resolution change needs a relaunch -- say so under the console */
    {
        DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
        Font_DrawText(d, 60.0f, 410.0f, "Resolution applies on next launch",
            FONT_SIZE_SMALL, dim, 0);
    }
}

/*---- render: Network (read-only, runtime info) -----------------------------*/

static void RenderNetwork(IDirect3DDevice8* d) {
    char modeRow[40], ipRow[40], subRow[40], gwRow[40], d1Row[40], d2Row[40], ipStr[20];
    const char* rows[7];
    int  n = 0;
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);

    strcpy(modeRow, "Mode      ");
    strcat(modeRow, (s_netMode == DD_NET_STATIC) ? "Static"
        : (s_netMode == DD_NET_DHCP_DNS) ? "DHCP + DNS" : "DHCP");
    rows[n++] = modeRow;

    if (s_netMode == DD_NET_STATIC) {
        FmtIp(s_netIp, ipStr);   strcpy(ipRow, "IP        "); strcat(ipRow, ipStr);  rows[n++] = ipRow;
        FmtIp(s_netMask, ipStr); strcpy(subRow, "Subnet    "); strcat(subRow, ipStr); rows[n++] = subRow;
        FmtIp(s_netGw, ipStr);   strcpy(gwRow, "Gateway   "); strcat(gwRow, ipStr);  rows[n++] = gwRow;
    }
    if (s_netMode == DD_NET_STATIC || s_netMode == DD_NET_DHCP_DNS) {
        FmtIp(s_netDns1, ipStr); strcpy(d1Row, "DNS 1     "); strcat(d1Row, ipStr); rows[n++] = d1Row;
        FmtIp(s_netDns2, ipStr); strcpy(d2Row, "DNS 2     "); strcat(d2Row, ipStr); rows[n++] = d2Row;
    }
    /* Pure DHCP: nothing here is editable, so instead show what the server
       actually assigned as read-only rows (laid out by DrawConsole exactly like
       any other row -- aligned, spaced, no overlap). They sit between Mode and
       Apply and render dimmed/non-selectable (selectable = NetRowCount() = 2). */
    if (s_netMode == DD_NET_DHCP) {
        strcpy(ipRow, "IP        "); strcat(ipRow, Net_Ip());      rows[n++] = ipRow;
        strcpy(subRow, "Mask      "); strcat(subRow, Net_Subnet());  rows[n++] = subRow;
        strcpy(gwRow, "Gateway   "); strcat(gwRow, Net_Gateway()); rows[n++] = gwRow;
        strcpy(d1Row, "DNS 1     "); strcat(d1Row, Net_Dns());     rows[n++] = d1Row;
        strcpy(d2Row, "DNS 2     "); strcat(d2Row, Net_Dns2());    rows[n++] = d2Row;
    }
    rows[n++] = "Apply";

    Chrome(d, "NETWORK", "L/R MODE   A EDIT/APPLY   B BACK");
    DrawPedestal(d);
    {
        /* Static / DHCP+DNS: the logical cursor maps 1:1 to array rows (all
           selectable). Pure DHCP: only Mode (row 0) and Apply are selectable,
           with read-only assigned-value rows in between -- so map the logical
           Apply (s_row==1) to the LAST array row, and dim the rows between. */
        int selArrayRow = s_row;
        int selectable = n;
        if (s_netMode == DD_NET_DHCP) {
            selArrayRow = (s_row == 0) ? 0 : (n - 1);
            selectable = 1;            /* only Mode in the in-line range... */
        }
        DrawConsole(d, rows, n, selArrayRow, selectable, 1);
    }

    /* live readout of what's actually active right now */
    {
        char live[48];
        strcpy(live, Net_LinkUp() ? "Link up   " : "No link   ");
        strcat(live, Net_Ip());
        Font_DrawText(d, 60.0f, 410.0f, live, FONT_SIZE_SMALL, dim, 0);
        if (s_netMsg) {
            DWORD ok = Theme_Color("accent", 0xFF7FE000);
            Font_DrawText(d, 60.0f, 428.0f,
                (s_netMsg == 1) ? "Applied -- reconnecting" : "Apply failed",
                FONT_SIZE_SMALL, (s_netMsg == 1) ? ok : dim, 0);
        }
    }

    if (Osk_IsOpen()) Osk_Draw(d);   /* numeric entry overlay on top */
}

/*---- render: FTP -----------------------------------------------------------*/

static void RenderFtp(IDirect3DDevice8* d) {
    DD_Settings* st = Data_Get();
    char svcRow[40], portRow[40], userRow[48], passRow[48], num[8];
    const char* rows[5];
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    int  i;

    strcpy(svcRow, "Service   "); strcat(svcRow, st->ftpEnabled ? "Enabled" : "Disabled");
    strcpy(portRow, "Port      "); IntToText(st->ftpPort, num); strcat(portRow, num);
    strcpy(userRow, "Username  "); strcat(userRow, st->ftpUser);
    /* mask the password */
    strcpy(passRow, "Password  ");
    { int n = 0; while (st->ftpPass[n]) n++; for (i = 0; i < n && i < 16; i++) strcat(passRow, "*"); }

    rows[0] = svcRow; rows[1] = portRow; rows[2] = userRow; rows[3] = passRow;
    rows[4] = "Apply";

    Chrome(d, "FTP", "A EDIT/TOGGLE   B BACK");
    DrawPedestal(d);
    DrawConsole(d, rows, 5, s_row, 5, 1);   /* Apply pinned to frame bottom */

    /* live status readout */
    {
        const char* ss;
        int s = Ftp_Status();
        char line[48];
        ss = (s == 3) ? "Transferring" : (s == 2) ? "Client connected"
            : (s == 1) ? "Listening" : "Stopped";
        strcpy(line, "Status    "); strcat(line, ss);
        Font_DrawText(d, 60.0f, 400.0f, line, FONT_SIZE_SMALL, dim, 0);
        if (Ftp_IsRunning() && Net_IsUp()) {
            char ipl[48];
            strcpy(ipl, "ftp://"); strcat(ipl, Net_Ip());
            Font_DrawText(d, 60.0f, 418.0f, ipl, FONT_SIZE_SMALL, dim, 0);
        }
    }

    if (Osk_IsOpen()) Osk_Draw(d);
}

/*---- render: Clock (editable fields + Set) ---------------------------------*/

static void RenderClock(IDirect3DDevice8* d) {
    char tRow[40], moRow[32], dRow[32], yRow[32], ntpRow[32], tzRow[64], num[8], hh[4], mm[4];
    const char* rows[8];

    Pad2(s_clk.hour, hh);
    Pad2(s_clk.min, mm);

    /* Time row: combined HH:MM; when selected, bracket the active part */
    strcpy(tRow, "Time      ");
    if (s_row == 0 && s_clkPart == 0) {
        strcat(tRow, "["); strcat(tRow, hh); strcat(tRow, "]:"); strcat(tRow, mm);
    }
    else if (s_row == 0 && s_clkPart == 1) {
        strcat(tRow, hh); strcat(tRow, ":["); strcat(tRow, mm); strcat(tRow, "]");
    }
    else {
        strcat(tRow, hh); strcat(tRow, ":"); strcat(tRow, mm);
    }

    strcpy(moRow, "Month     "); Pad2(s_clk.mon, num); strcat(moRow, num);
    strcpy(dRow, "Day       "); Pad2(s_clk.day, num); strcat(dRow, num);
    strcpy(yRow, "Year      "); IntToText(s_clk.year, num); strcat(yRow, num);

    strcpy(ntpRow, "Net Time  "); strcat(ntpRow, Time_NtpEnabled() ? "On" : "Off");
    {
        char off[12];
        Tz_OffsetStr(Time_TzIndex(), off, sizeof(off));
        strcpy(tzRow, "Zone  ");
        strcat(tzRow, Tz_Name(Time_TzIndex()));
        strcat(tzRow, "  ");
        strcat(tzRow, off);
    }

    rows[0] = tRow; rows[1] = moRow; rows[2] = dRow; rows[3] = yRow;
    rows[4] = ntpRow; rows[5] = tzRow; rows[6] = "Sync Now"; rows[7] = "Apply";

    Chrome(d, "CLOCK", "L/R ADJUST   A SWITCH/APPLY   B BACK");
    DrawPedestal(d);
    DrawConsole(d, rows, 8, s_row, 8, 1);   /* Apply pinned to frame bottom */

    if (Rtc_Present()) {
        DWORD acc = Theme_Color("accent", 0xFF7FE000);
        Font_DrawText(d, 360.0f, 18.0f, "X-RTC Detected", FONT_SIZE_SMALL, acc, 0);
    }

    if (s_clkMsg) {
        DWORD ok = Theme_Color("accent", 0xFF7FE000);
        DWORD bad = Theme_Color("text_dim", 0xFF7FA060);
        const char* m =
            (s_clkMsg == 1) ? "Clock updated" :
            (s_clkMsg == 3) ? "Synced from internet" :
            (s_clkMsg == 4) ? "Sync failed -- check network" : "Invalid date/time";
        Font_DrawText(d, 60.0f, 410.0f, m, FONT_SIZE_SMALL,
            (s_clkMsg == 1 || s_clkMsg == 3) ? ok : bad, 0);
    }
}

/*---- render: Update --------------------------------------------------------*/

static void UpdProgBar(IDirect3DDevice8* d, float x, float y, float w, float h, int pct) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    int   dr = (int)((dim >> 16) & 0xFF), dg = (int)((dim >> 8) & 0xFF), db = (int)(dim & 0xFF);
    float fw;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    fw = w * (float)pct / 100.0f;
    UI_FillRect(x, y, w, h, UI_ARGB(90, dr, dg, db));               /* themed track */
    if (fw > 0) UI_FillRect(x, y, fw, h, UI_ARGB(220, ar, ag, ab));
    (void)d;
}

static void RenderUpdate(IDirect3DDevice8* d) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    const Texture* bar = Theme_Asset("bar_footer");
    int   st = Upd_State();
    const char* status; DWORD sc = text;
    const char* hint = "B BACK";
    char line[96];
    /* slim status bar, centered on screen (like the bottom bar) */
    float bx = 112.0f, by = 180.0f, bw = 416.0f, bh = 32.0f;

    Chrome(d, "UPDATE", "");

    /* installed version (top) -- centered across the full screen width */
    line[0] = 0; lstrcatA(line, "Installed: "); lstrcatA(line, Upd_LocalVersion());
    Font_DrawTextCentered(d, 0.0f, 84.0f, 640.0f, line, FONT_SIZE_SMALL, dim);

    /* three simple states: checking / update found / no update found
       (plus the in-progress + done cases) */
    switch (st) {
    case UPD_CHECKING:    status = "Checking for update";  sc = glow;   hint = "B CANCEL"; break;
    case UPD_AVAILABLE:   status = "Update found";         sc = glow;   hint = "A DOWNLOAD   B BACK"; break;
    case UPD_UPTODATE:    status = "No update found";      sc = dim;    hint = "A RE-CHECK   B BACK"; break;
    case UPD_DOWNLOADING: status = "Downloading update";   sc = glow;   hint = ""; break;
    case UPD_EXTRACTING:  status = "Extracting update";    sc = glow;   hint = ""; break;
    case UPD_DONE:        status = "Update complete";      sc = accent; hint = "A RELAUNCH"; break;
    case UPD_ERROR:       status = "No update found";      sc = dim;    hint = "A RE-CHECK   B BACK"; break;
    default:              status = "Press A to check";     sc = text;   hint = "A CHECK   B BACK"; break;
    }

    /* slim bar holds the status line, text centered on the bar's span */
    if (bar) UI_DrawSprite(bar, bx, by, bw, bh, 0xFFFFFFFF, 0);
    Font_DrawTextCentered(d, bx, by + 8.0f, bw, status, FONT_SIZE_SMALL, sc);

    /* if an update was found, show the target version under the bar */
    if (st == UPD_AVAILABLE && Upd_RemoteVersion()[0]) {
        line[0] = 0; lstrcatA(line, "Version "); lstrcatA(line, Upd_RemoteVersion());
        Font_DrawTextCentered(d, bx, by + 44.0f, bw, line, FONT_SIZE_SMALL, text);
    }

    /* progress bar (download / extract / done) -- in a matching slim bar below */
    if (st == UPD_DOWNLOADING || st == UPD_EXTRACTING || st == UPD_DONE) {
        int pct = (st == UPD_DONE) ? 100 : Upd_Progress();
        if (bar) UI_DrawSprite(bar, bx, by + 44.0f, bw, bh, 0xFFFFFFFF, 0);
        UpdProgBar(d, bx + 16.0f, by + 53.0f, bw - 32.0f, 14.0f, pct);
    }

    /* footer hint -- centered across the full bottom bar */
    {
        const Texture* foot = Theme_Asset("bar_footer");
        if (foot) UI_DrawSprite(foot, 8.0f, 442.0f, 624.0f, 32.0f, 0xFFFFFFFF, 0);
        if (hint[0]) Font_DrawTextCentered(d, 8.0f, 449.0f, 624.0f, hint, FONT_SIZE_SMALL, text);
    }
}

/*---- render: Theme ---------------------------------------------------------*/

static void RenderTheme(IDirect3DDevice8* d) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* menu = Theme_Asset("frame_menu_v");
    float menuX = 352.0f, menuY = 48.0f, rowY0 = 96.0f, rowDY = 28.0f;
    int   vis = 11, top, i, count = Theme_Count();

    Chrome(d, "THEME", "A APPLY   B BACK");
    DrawPedestal(d);

    top = 0;
    if (s_themeCursor >= vis) top = s_themeCursor - vis + 1;

    Iso_Begin();
    if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
    for (i = 0; i < vis; i++) {
        int idx = top + i;
        const char* nm;
        DWORD c;
        if (idx >= count) break;
        nm = Theme_NameAt(idx);
        if (!nm) continue;
        if (idx == s_themeCursor)
            Iso_FillRect(menuX + 18.0f, rowY0 + rowDY * (float)i - 4.0f, 210.0f, 26.0f,
                UI_ARGB(90, ar, ag, ab), 1);
        c = (idx == s_themeCursor) ? glow : (idx == 0 ? accent : text);
        Font_DrawTextIso(d, menuX + 28.0f, rowY0 + rowDY * (float)i,
            nm, FONT_SIZE_SMALL, c);
    }
    Iso_End();

    if (s_themeMsg) {
        DWORD ok = Theme_Color("accent", 0xFF7FE000);
        Font_DrawText(d, 60.0f, 410.0f,
            (s_themeMsg == 1) ? "Theme applied" : "Load failed - using default",
            FONT_SIZE_SMALL, (s_themeMsg == 1) ? ok : dim, 0);
    }
}

/*---- render: Font ----------------------------------------------------------*/

static void RenderFont(IDirect3DDevice8* d) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* menu = Theme_Asset("frame_menu_v");
    float menuX = 352.0f, menuY = 48.0f, rowY0 = 96.0f, rowDY = 28.0f;
    int   vis = 11, top, i;

    Chrome(d, "FONT", "A APPLY   B BACK");
    DrawPedestal(d);

    /* scroll so the cursor stays visible */
    top = 0;
    if (s_fontCursor >= vis) top = s_fontCursor - vis + 1;

    Iso_Begin();
    if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
    for (i = 0; i < vis; i++) {
        int idx = top + i;
        const char* nm;
        DWORD c;
        if (idx >= s_fontCount) break;
        nm = s_fonts[idx];
        if (idx == s_fontCursor)
            Iso_FillRect(menuX + 18.0f, rowY0 + rowDY * (float)i - 4.0f, 210.0f, 26.0f,
                UI_ARGB(90, ar, ag, ab), 1);
        c = (idx == s_fontCursor) ? glow : (idx == 0 ? accent : text);
        Font_DrawTextIso(d, menuX + 28.0f, rowY0 + rowDY * (float)i,
            nm, FONT_SIZE_SMALL, c);
    }
    Iso_End();

    if (s_fontMsg) {
        DWORD ok = Theme_Color("accent", 0xFF7FE000);
        Font_DrawText(d, 60.0f, 410.0f,
            (s_fontMsg == 1) ? "Font applied" : "Load failed - using Default",
            FONT_SIZE_SMALL, (s_fontMsg == 1) ? ok : dim, 0);
    }
}

/*---- visual control rows (RGB / OXFP) --------------------------------------
   Draw helpers for the row-descriptor control screens. The CtrlRow type, the
   RR_* enum, the layout constants, and the row builders are declared earlier
   (above UpdateAccessories) so both the Update and Render paths can see them. */

   /* a value bar: dim track + accent fill proportional to v/255 */
static void DrawValBar(float vx, float vy, int v, int ar, int ag, int ab) {
    float w = CR_WIDGETW, h = 12.0f;
    float fill = (v < 0 ? 0 : v > 255 ? 255 : v) / 255.0f * w;
    Iso_FillRect(vx, vy, w, h, UI_ARGB(90, ar, ag, ab), 0);          /* track   */
    if (fill > 1.0f) Iso_FillRect(vx, vy, fill, h, UI_ARGB(230, ar, ag, ab), 0);
}

/* a color swatch with a thin themed border. fill is the literal LED colour
   (that's the point of the swatch); the border uses the theme so the frame
   around it matches whatever theme is loaded. */
static void DrawSwatch(float vx, float vy, unsigned long rgb, DWORD border) {
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    float w = 40.0f, h = 14.0f;
    Iso_FillRect(vx - 1.0f, vy - 1.0f, w + 2.0f, h + 2.0f, border, 0);
    Iso_FillRect(vx, vy, w, h, UI_ARGB(255, r, g, b), 0);
}

/* Draw a mode/anim NAME value so it always fits inside the frame: full MEDIUM
   when it fits the name column, auto-dropping to SMALL for the long names, and
   clipped at the frame interior so it can never cross the right border. */
static void DrawNameValue(IDirect3DDevice8* d, float ry, const char* s, DWORD c) {
    /* Short names sit at the bar column, aligned with the other value widgets
       (Animation, Plasma, Solid...). A long name that would clip there shifts
       left into the wider name column so it shows in full -- still at MEDIUM if it
       fits, dropping to SMALL only for the very longest ("UNSC/Covenant"). Both
       paths clip at the frame interior so the text never crosses the border. */
    float barMax = CR_VALRIGHT - CR_WIDGETX;
    if ((float)Font_MeasureText(s, FONT_SIZE_MEDIUM) <= barMax) {
        Font_DrawTextIsoClip(d, CR_WIDGETX, ry + 2.0f, s, FONT_SIZE_MEDIUM, c, barMax);
        return;
    }
    {
        float wideMax = CR_VALRIGHT - CR_NAMEVALX;
        int   size = FONT_SIZE_MEDIUM;
        if ((float)Font_MeasureText(s, FONT_SIZE_MEDIUM) > wideMax) size = FONT_SIZE_SMALL;
        Font_DrawTextIsoClip(d, CR_NAMEVALX, ry + 2.0f, s, size, c, wideMax);
    }
}

/* Draw a built control-row list with value text + bars + swatches, selection
   glow, and adaptive spacing to fit the frame (same scheme as DrawConsole). */
static void DrawControlRows(IDirect3DDevice8* d, const CtrlRow* rows, int count,
    int sel, int present, int isOxfp) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* menu = Theme_Asset("frame_menu_v");
    float listBottom = CR_MENUY + 384.0f - 24.0f;
    float rowDY = 42.0f;                       /* fixed, comfortable row pitch */
    int   fit = (int)((listBottom - CR_ROWY0) / rowDY);   /* rows that fit on screen */
    int   first = 0;                           /* index of the first visible row */
    int   i;

    if (fit < 1) fit = 1;

    /* Scroll instead of compressing: when the list is taller than the panel,
       keep the rows at full pitch and slide the window so the selected row
       stays in view (roughly centered, pinned at the ends). This keeps the
       bottom rows -- Save / Reset -- reachable instead of clipping off. */
    if (count > fit) {
        first = sel - fit / 2;
        if (first < 0) first = 0;
        if (first > count - fit) first = count - fit;
    }

    Iso_Begin();
    if (menu) Iso_DrawPanel(menu, CR_MENUX, CR_MENUY, 272.0f, 384.0f, 0xFFFFFFFF, 0);

    /* selection glow (only when present, so absent screens read as inert) */
    if (present && sel >= 0 && sel < count) {
        float gy = CR_ROWY0 + rowDY * (float)(sel - first) - 6.0f;
        Select_Begin(0x5200 + s_view, gy);
        Select_DrawGlow(CR_MENUX + 18.0f, gy, 210.0f, 30.0f, UI_ARGB(110, ar, ag, ab));
    }

    for (i = first; i < first + fit && i < count; i++) {
        const CtrlRow* rr = &rows[i];
        float ry = CR_ROWY0 + rowDY * (float)(i - first);
        DWORD c = !present ? dim : (i == sel) ? glow : text;
        unsigned long sw;

        Font_DrawTextIsoClip(d, CR_LABELX, ry + 2.0f, rr->label, FONT_SIZE_MEDIUM, c, 120.0f);

        switch (rr->type) {
        case RR_MODE:
            /* name column + auto-shrink so long names ("UNSC/Covenant",
               "Palette Cycle") render in full instead of clipping at the bar column */
            DrawNameValue(d, ry, isOxfp ? Oxfp_ModeName(s_oxfpMode) : Rgb_ModeName(s_rgbMode), c);
            break;
        case RR_ANIMMODE:
            DrawNameValue(d, ry, Oxfp_AnimName(s_oxfpAnim), c);
            break;
        case RR_BRIGHT:
            DrawValBar(CR_WIDGETX, ry + 4.0f, isOxfp ? s_oxfpBright : s_rgbBright, ar, ag, ab);
            break;
        case RR_SPEED:
            DrawValBar(CR_WIDGETX, ry + 4.0f, s_rgbSpeed, ar, ag, ab);
            break;
        case RR_INTENSITY:
            DrawValBar(CR_WIDGETX, ry + 4.0f, s_rgbIntensity, ar, ag, ab);
            break;
        case RR_ANIMSPEED:
            DrawValBar(CR_WIDGETX, ry + 4.0f, s_oxfpAnimSpeed, ar, ag, ab);
            break;
        case RR_PALCOUNT: {
            char nb[4]; nb[0] = (char)('0' + s_rgbPalCount); nb[1] = 0;
            Font_DrawTextIsoClip(d, CR_WIDGETX, ry + 2.0f, nb, FONT_SIZE_MEDIUM, c, 40.0f);
            break;
        }
        case RR_COLOR:
            if (isOxfp) sw = k_palette[s_oxfpStatusIx[rr->idx]];
            else        sw = k_palette[s_rgbColIx[rr->idx]];
            DrawSwatch(CR_WIDGETX, ry + 2.0f, sw, dim);
            break;
        case RR_ANIMCOLOR:
            DrawSwatch(CR_WIDGETX, ry + 2.0f, k_palette[s_oxfpAnimIx[rr->idx]], dim);
            break;
        default: /* RR_SAVE / RR_RESET / RR_IDENTIFY: label only */
            break;
        }
    }

    /* scroll hints: a chevron when rows are hidden above / below the window */
    if (first > 0)
        Font_DrawTextIsoClip(d, CR_MENUX + 128.0f, CR_ROWY0 - 22.0f, "^",
            FONT_SIZE_MEDIUM, dim, 24.0f);
    if (first + fit < count)
        Font_DrawTextIsoClip(d, CR_MENUX + 128.0f, listBottom - 4.0f, "v",
            FONT_SIZE_MEDIUM, dim, 24.0f);

    Iso_End();
}

/*---- RenderAccessories ------------------------------------------------------
   Device list, or the selected device's screen. */
static void RenderAccessories(IDirect3DDevice8* d) {
    /* ----- device list ----- */
    if (s_accDev < 0) {
        const char* rows[ACC_DEV_COUNT];
        char lcdRow[40], tdRow[40], rgbRow[40], oxRow[40], xvRow[40];
        /* uniform detection status across the detectable devices. LCD only
           probes the SMBus when enabled, so when it's off we say "Off" rather
           than claiming nothing's there. */
        strcpy(lcdRow, "LCD       ");
        strcat(lcdRow, !Lcd_Enabled() ? "Off" : (Lcd_IsPresent() ? "Detected" : "Not found"));
        strcpy(tdRow, "Type-D    ");  strcat(tdRow, TypeD_Enabled() ? "On" : "Off");
        strcpy(rgbRow, "XBOX-RGB  ");  strcat(rgbRow, Udp_Present(UDP_DEV_RGB) ? "Detected" : "Not found");
        strcpy(oxRow, "OXFP      ");  strcat(oxRow, Udp_Present(UDP_DEV_OXFP) ? "Detected" : "Not found");
        strcpy(xvRow, "X-View    ");  strcat(xvRow, !XView_IsEnabled() ? "Off" : (XView_IsReady() ? "Connected" : "On"));
        rows[ACC_DEV_LCD] = lcdRow;
        rows[ACC_DEV_TYPED] = tdRow;
        rows[ACC_DEV_RGB] = rgbRow;
        rows[ACC_DEV_OXFP] = oxRow;
        rows[ACC_DEV_XVIEW] = xvRow;
        Chrome(d, "ACCESSORIES", "A OPEN   B BACK");
        DrawPedestal(d);
        DrawConsole(d, rows, ACC_DEV_COUNT, s_accRow, ACC_DEV_COUNT, 0);
        return;
    }

    /* ----- X-View screen ----- */
    if (s_accDev == ACC_DEV_XVIEW) {
        char enRow[40], ivRow[40], brRow[40], pnRow[40], num[8];
        char pTemps[40], pMem[40], pDisk[40], pNet[40], pFtp[40], pClk[40], pNow[40];
        const char* rows[11];
        int k, pages;
        strcpy(enRow, "Enabled   ");
        if (!XView_IsEnabled())   strcat(enRow, "No");
        else if (XView_IsReady()) strcat(enRow, "Yes (connected)");
        else                      strcat(enRow, "Yes (no panel)");
        k = IntToText(XView_IntervalMs() / 1000, num); num[k] = 's'; num[k + 1] = 0;
        strcpy(ivRow, "Interval  "); strcat(ivRow, num);
        k = IntToText(XView_Brightness() * 100 / 255, num); num[k] = '%'; num[k + 1] = 0;
        strcpy(brRow, "Bright    "); strcat(brRow, num);
        strcpy(pnRow, "Panel     "); strcat(pnRow, XView_Panel() == XV_PANEL_A ? "Bar 284x76" : "320x240");
        pages = XView_Pages();
        strcpy(pTemps, "Temps     "); strcat(pTemps, (pages & LCD_PAGE_TEMPS) ? "On" : "Off");
        strcpy(pMem, "Memory    "); strcat(pMem, (pages & LCD_PAGE_MEM) ? "On" : "Off");
        strcpy(pDisk, "Disk      "); strcat(pDisk, (pages & LCD_PAGE_DISK) ? "On" : "Off");
        strcpy(pNet, "Network   "); strcat(pNet, (pages & LCD_PAGE_NET) ? "On" : "Off");
        strcpy(pFtp, "FTP       "); strcat(pFtp, (pages & LCD_PAGE_FTP) ? "On" : "Off");
        strcpy(pClk, "Clock     "); strcat(pClk, (pages & LCD_PAGE_CLOCK) ? "On" : "Off");
        strcpy(pNow, "Now Play  "); strcat(pNow, (pages & LCD_PAGE_NOWPLAYING) ? "On" : "Off");
        rows[0] = enRow; rows[1] = ivRow; rows[2] = brRow; rows[3] = pnRow;
        rows[4] = pTemps; rows[5] = pMem; rows[6] = pDisk; rows[7] = pNet;
        rows[8] = pFtp; rows[9] = pClk; rows[10] = pNow;
        Chrome(d, "X-VIEW", "A TOGGLE  L/R ADJUST  B BACK");
        DrawPedestal(d);
        DrawConsole(d, rows, 11, s_accRow, 11, 0);
        return;
    }

    /* ----- LCD screen ----- */
    if (s_accDev == ACC_DEV_LCD) {
        char enRow[40], adRow[40], ivRow[40], brRow[40], coRow[40];
        char pTemps[40], pMem[40], pDisk[40], pNet[40], pFtp[40], pClk[40], pNow[40];
        const char* rows[12]; char num[8]; int k, pages;

        strcpy(enRow, "Enabled   ");
        if (!Lcd_Enabled())          strcat(enRow, "No");
        else if (Lcd_IsPresent())    strcat(enRow, "Yes (detected)");
        else                         strcat(enRow, "Yes (no panel)");
        strcpy(adRow, "Address   "); strcat(adRow, Lcd_AddrChoice() == LCD_ADDR_3D ? "0x3D" : "0x3C");
        k = IntToText(Lcd_IntervalMs() / 1000, num); num[k] = 's'; num[k + 1] = 0;
        strcpy(ivRow, "Interval  "); strcat(ivRow, num);
        k = IntToText(Lcd_Brightness() * 100 / 255, num); num[k] = '%'; num[k + 1] = 0;
        strcpy(brRow, "Bright    "); strcat(brRow, num);
        strcpy(coRow, "Compat    "); strcat(coRow, Lcd_CompatMode() ? "On" : "Off");

        pages = Lcd_Pages();
        strcpy(pTemps, "Temps     "); strcat(pTemps, (pages & LCD_PAGE_TEMPS) ? "On" : "Off");
        strcpy(pMem, "Memory    "); strcat(pMem, (pages & LCD_PAGE_MEM) ? "On" : "Off");
        strcpy(pDisk, "Disk      "); strcat(pDisk, (pages & LCD_PAGE_DISK) ? "On" : "Off");
        strcpy(pNet, "Network   "); strcat(pNet, (pages & LCD_PAGE_NET) ? "On" : "Off");
        strcpy(pFtp, "FTP       "); strcat(pFtp, (pages & LCD_PAGE_FTP) ? "On" : "Off");
        strcpy(pClk, "Clock     "); strcat(pClk, (pages & LCD_PAGE_CLOCK) ? "On" : "Off");
        strcpy(pNow, "Now Play  "); strcat(pNow, (pages & LCD_PAGE_NOWPLAYING) ? "On" : "Off");

        rows[0] = enRow; rows[1] = adRow; rows[2] = ivRow; rows[3] = brRow; rows[4] = coRow;
        rows[5] = pTemps; rows[6] = pMem; rows[7] = pDisk; rows[8] = pNet; rows[9] = pFtp; rows[10] = pClk;
        rows[11] = pNow;

        Chrome(d, "LCD", "A TOGGLE  L/R ADJUST  B BACK");
        DrawPedestal(d);
        DrawConsole(d, rows, 12, s_accRow, 12, 0);
        return;
    }

    /* ----- Type-D screen ----- */
    if (s_accDev == ACC_DEV_TYPED) {
        const char* rows[5];
        char enRow[40];
        char xlRow[40];
        char tdaRow[40];
        char detRow[48];
        int  idd, any;
        int  xlPresent = Udp_TypeDPresent(5);
        int  ctrlPresent = (Udp_TypeDPresent(1) || Udp_TypeDPresent(2) ||
            Udp_TypeDPresent(3) || Udp_TypeDPresent(4));
        int  anyPresent = (xlPresent || ctrlPresent);
        DWORD dis = 0;                       /* greyed rows when device absent   */
        if (!xlPresent)   dis |= (1u << 1);  /* XL Art                           */
        if (!ctrlPresent) dis |= (1u << 2);  /* Type-D Art                       */
        if (!anyPresent)  dis |= (1u << 3);  /* Resume                           */
        strcpy(enRow, "Enabled    "); strcat(enRow, TypeD_Enabled() ? "Yes" : "No");
        /* "XL Art" -> XL (id 5, 480x480); "Type-D Art" -> regular Type-D units
           (ids 1-4, 240x240). When a device class isn't detected its row is
           greyed (unselectable) and shown as "No" -- a display flag only; the
           stored dc.dat preference is NOT written, so it returns intact when the
           device comes back. */
        strcpy(xlRow, "XL Art     "); strcat(xlRow, !xlPresent ? "No" : (Dc_TypeDArtEnabled() ? "Yes" : "No"));
        strcpy(tdaRow, "Type-D Art "); strcat(tdaRow, !ctrlPresent ? "No" : (Dc_TypeDCtrlArtEnabled() ? "Yes" : "No"));
        /* per-id roster of present units: XL + P1..P4 */
        strcpy(detRow, "Units: ");
        any = 0;
        for (idd = 1; idd <= 5; idd++) {
            if (!Udp_TypeDPresent(idd)) continue;
            if (any) strcat(detRow, " ");
            if (idd == 5) strcat(detRow, "XL");
            else { char p[3]; p[0] = 'P'; p[1] = (char)('0' + idd); p[2] = 0; strcat(detRow, p); }
            any = 1;
        }
        if (!any) strcat(detRow, "none");
        rows[0] = enRow;
        rows[1] = xlRow;
        rows[2] = tdaRow;
        rows[3] = "Resume Slideshow";
        rows[4] = detRow;
        Chrome(d, "TYPE-D", "A SELECT  B BACK");
        DrawPedestal(d);
        DrawConsole(d, rows, 5, s_accRow, 4, 0, dis);   /* 0-3 selectable; 4 = roster; dis = greyed */
        return;
    }

    /* ----- XBOX-RGB screen ----- */
    if (s_accDev == ACC_DEV_RGB) {
        CtrlRow rows[14]; int n, present = Udp_Present(UDP_DEV_RGB);
        n = BuildRgbRows(rows);
        Chrome(d, "XBOX-RGB", present ? "L/R ADJUST  A SELECT  B BACK" : "Not detected   B BACK");
        DrawPedestal(d);
        DrawControlRows(d, rows, n, s_accRow, present, 0);
        return;
    }

    /* ----- OXFP screen ----- */
    if (s_accDev == ACC_DEV_OXFP) {
        CtrlRow rows[14]; int n, present = Udp_Present(UDP_DEV_OXFP);
        n = BuildOxfpRows(rows);
        Chrome(d, "OXFP", present ? "L/R ADJUST  A SELECT  B BACK" : "Not detected   B BACK");
        DrawPedestal(d);
        DrawControlRows(d, rows, n, s_accRow, present, 1);
        return;
    }
}

void Settings_Render(void) {
    IDirect3DDevice8* d = Gfx_Device();
    if (s_view < 0) { RenderList(d);    return; }
    if (s_view == CAT_ABOUT) { RenderAbout(d);   return; }
    if (s_view == CAT_AUDIO) { RenderAudio(d);   return; }
    if (s_view == CAT_FAN) { RenderFan(d);     return; }
    if (s_view == CAT_VIDEO) { RenderVideo(d);   return; }
    if (s_view == CAT_NETWORK) { RenderNetwork(d); return; }
    if (s_view == CAT_FTP) { RenderFtp(d);     return; }
    if (s_view == CAT_CLOCK) { RenderClock(d);   return; }
    if (s_view == CAT_FONT) { RenderFont(d);    return; }
    if (s_view == CAT_THEME) { RenderTheme(d);   return; }
    if (s_view == CAT_UPDATE) { RenderUpdate(d);  return; }
    if (s_view == CAT_ACCESSORIES) { RenderAccessories(d); return; }
    RenderPlaceholder(d, k_items[s_view]);
}

/* Render pump invoked by the updater during the blocking download (every
   ~64KB). Draws one full frame so the Update panel's progress bar advances on
   screen even though DoDownload() is blocking (XbDiag's render-callback
   pattern). Forces the Update category so the bar is what's shown. */
static void SettingsUpdRenderPump(void) {
    Gfx_BeginFrame(Theme_BG());
    Settings_Render();
    Gfx_EndFrame();
}