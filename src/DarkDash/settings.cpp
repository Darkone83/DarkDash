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
#include "dd_update.h"
#include "dd_calib.h"
#include "dd_select.h"
#include "Settings.h"

/* forward decls for the download render-pump (defined after Settings_Render) */
void Settings_Render(void);
static void SettingsUpdRenderPump(void);
#include "dd_version.h"

#define SET_COUNT 10
#define MUSIC_DIR "D:\\audio\\music"
#define MUSIC_MAX 64

enum {
    CAT_NETWORK = 0, CAT_FTP, CAT_VIDEO, CAT_AUDIO,
    CAT_FAN, CAT_CLOCK, CAT_THEME, CAT_FONT, CAT_UPDATE, CAT_ABOUT
};

static const char* const k_items[SET_COUNT] = {
    "Network", "FTP", "Video", "Audio", "Fan", "Clock", "Theme", "Font", "Update", "About"
};
static const char* const k_icon[SET_COUNT] = {
    "s2_020.png", "s2_018.png", "s2_005.png", "s2_004.png", "s2_010.png",
    "s2_008.png", "s2_006.png", "s2_002.png", "s2_019.png", "s2_011.png"
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
static int s_fxRow = 0;    /* cursor within the Effects sub-menu   */
static const int k_fxBits[4] = { DD_FX_SCANLINES, DD_FX_SELECT, DD_FX_IDLE, DD_FX_EDGE };
static const char* const k_fxNames[4] = { "Scanlines", "Selection FX", "Idle Motion", "Edge Flash" };

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
    int selRow, int selectable, int pinLast) {
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* menu = Theme_Asset("frame_menu_v");
    float menuX = 352.0f, menuY = 48.0f, rowY0 = 96.0f, rowDY = 42.0f;
    float bottomY = menuY + 384.0f - 50.0f;   /* pinned action row, near frame foot */
    int i;

    Iso_Begin();
    if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
    if (selRow >= 0 && (selRow < selectable || (pinLast && selRow == count - 1))) {
        float gy = ((pinLast && selRow == count - 1) ? bottomY : rowY0 + rowDY * (float)selRow) - 6.0f;
        Select_Begin(0x5000 + s_view, gy);
        Select_DrawGlow(menuX + 18.0f, gy, 210.0f, 34.0f, UI_ARGB(110, ar, ag, ab));
    }
    for (i = 0; i < count; i++) {
        /* the pinned last row is always the action row (Apply/Set) -- keep it
           bright even when it falls outside the in-line selectable range, as in
           pure-DHCP where the rows between Mode and Apply are read-only. */
        int isAction = (pinLast && i == count - 1);
        DWORD c = (i == selRow) ? glow
            : (isAction) ? text
            : (i >= selectable) ? dim : text;
        float ry = isAction ? bottomY : rowY0 + rowDY * (float)i;
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
        if (pressed & BTN_DPAD_DOWN) { if (s_mp3Cursor < s_mp3Count - 1) { s_mp3Cursor++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_mp3Cursor > 0) { s_mp3Cursor--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & BTN_A) {
            if (s_mp3Count > 0) {
                char full[260];
                strcpy(full, MUSIC_DIR); strcat(full, "\\"); strcat(full, s_mp3[s_mp3Cursor]);
                st->musicCustom = 1;
                strncpy(st->musicPath, full, DD_MUSIC_PATH_MAX - 1); st->musicPath[DD_MUSIC_PATH_MAX - 1] = 0;
                Audio_StopMusic(); Audio_SetMusicPath(full);
                Audio_SetMusicVolume(st->musicVolume); Audio_StartMusic(1);
                Data_Save(); Audio_PlaySfx(SFX_SELECT);
            }
            s_audioPick = 0;
        }
        if (pressed & BTN_X) {
            st->musicCustom = 0; st->musicPath[0] = 0;
            Audio_StopMusic(); Audio_SetMusicPath(0);
            Audio_SetMusicVolume(st->musicVolume); Audio_StartMusic(1);
            Data_Save(); Audio_PlaySfx(SFX_ALT); s_audioPick = 0;
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

static void UpdateVideo(WORD pressed) {
    DD_Settings* st = Data_Get();

    /* ---- Effects sub-menu: toggle the four effect flags ---- */
    if (s_fxSub) {
        if (pressed & BTN_DPAD_DOWN) { if (s_fxRow < 3) { s_fxRow++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_fxRow > 0) { s_fxRow--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & (BTN_DPAD_LEFT | BTN_DPAD_RIGHT | BTN_A)) {
            st->fxFlags ^= k_fxBits[s_fxRow];      /* flip the bit */
            if (st->fxFlags == 0) st->fxFlags = 0; /* allow all-off in-session */
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

    Chrome(d, "SETTINGS", "A OPEN   B BACK");
    DrawPedestal(d);

    Iso_Begin();
    if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
    {
        float gy = rowY0 + rowDY * (float)s_cursor - 6.0f;
        Select_Begin(0x5001, gy);
        Select_DrawGlow(menuX + 18.0f, gy, 210.0f, 32.0f, UI_ARGB(110, ar, ag, ab));
    }
    for (i = 0; i < SET_COUNT; i++) {
        DWORD c = (i == s_cursor) ? glow : text;
        Font_DrawTextIso(d, menuX + 30.0f, rowY0 + rowDY * (float)i + 4.0f, k_items[i], FONT_SIZE_MEDIUM, c);
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
    strcpy(lines[nLines++], "CPU       Pentium III  733 MHz");
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
        int i, vis = (s_mp3Count < 9) ? s_mp3Count : 9;
        Chrome(d, "SELECT MUSIC", "A USE   X DEFAULT   B BACK");
        DrawPedestal(d);
        Iso_Begin();
        if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
        if (s_mp3Count == 0) {
            Font_DrawTextIso(d, menuX + 28.0f, rowY0, "No .mp3 files", FONT_SIZE_MEDIUM, dim);
        }
        else {
            float gy = rowY0 + rowDY * (float)s_mp3Cursor - 6.0f;
            Iso_FillRect(menuX + 16.0f, gy, 214.0f, 30.0f, UI_ARGB(110, ar, ag, ab), 1);
            for (i = 0; i < vis; i++) {
                DWORD c = (i == s_mp3Cursor) ? glow : text;
                Font_DrawTextIso(d, menuX + 26.0f, rowY0 + rowDY * (float)i + 2.0f, s_mp3[i], FONT_SIZE_SMALL, c);
            }
        }
        Iso_End();
        return;
    }

    k = IntToText(st->musicVolume, num); num[k] = '%'; num[k + 1] = 0;
    strcpy(volRow, "Volume    "); strcat(volRow, num);
    strcpy(trkRow, "Track     ");
    strcat(trkRow, (st->musicCustom && st->musicPath[0]) ? s_mp3[0] : "Built-in");
    /* show the custom file's name if set */
    if (st->musicCustom && st->musicPath[0]) {
        const char* p = st->musicPath; const char* base = p;
        while (*p) { if (*p == '\\') base = p + 1; p++; }
        strcpy(trkRow, "Track     "); strcat(trkRow, base);
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

/*---- render: placeholder (still launcher-style) ----------------------------*/

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
        char fxRows[4][40];
        const char* rp[4];
        int i;
        for (i = 0; i < 4; i++) {
            int onoff = (st->fxFlags & k_fxBits[i]) ? 1 : 0;
            strcpy(fxRows[i], k_fxNames[i]);
            /* pad to a column, then On/Off */
            while ((int)strlen(fxRows[i]) < 14) strcat(fxRows[i], " ");
            strcat(fxRows[i], onoff ? "On" : "Off");
            rp[i] = fxRows[i];
        }
        Chrome(d, "EFFECTS", "A TOGGLE   B BACK");
        DrawPedestal(d);
        DrawConsole(d, rp, 4, s_fxRow, 4, 0);
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
    float fw;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    fw = w * (float)pct / 100.0f;
    UI_FillRect(x, y, w, h, UI_ARGB(60, 80, 80, 80));
    if (fw > 0) UI_FillRect(x, y, fw, h, UI_ARGB(220, ar, ag, ab));
    (void)d; (void)dim;
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