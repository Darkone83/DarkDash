/*---------------------------------------------------------------------------
    main.cpp -- DarkDash entry point + isometric splash.

    Layering:
      - ambient green glow (flat, procedural) behind everything
      - orb drawn FLAT (it is pre-baked iso; the camera would double-tilt it)
      - header + menu ride the iso plane (chrome + text + selection fill)
      - footer status bar drawn FLAT (stable/readable; can tilt inward later)

    Controls:
      D-pad up/down   move selection
      WHITE + D-pad   live-tune the iso tilt (pitch / yaw)
      LT + RT + BACK  exit to system dashboard
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <math.h>
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
#include "dd_backdrop.h"
#include "dd_swing.h"
#include "dd_mount.h"
#include "dd_disc.h"
#include "dd_egg.h"
#include "dd_calib.h"
#include "dd_select.h"
#include "dd_fx.h"
#include "dd_savemgr.h"
#include "dd_recents.h"
#include "dd_paths.h"
#include "dd_time.h"
#include "dd_ntp.h"
#include "dd_net.h"
#include "dd_ftp.h"
#include "dd_lcd.h"
#include "dd_udp.h"
#include "dd_typed.h"
#include "dd_oxfp.h"
#include "dd_rgb.h"
#include "dd_screensaver.h"
#include "xboxinternals.h"
#include "dd_sysinfo.h"
#include "dd_launcher.h"
#include "Applications.h"
#include "Games.h"
#include "Homebrew.h"
#include "Emulators.h"
#include "FileMan.h"
#include "Settings.h"

#define DARKDASH_THEMES_DIR "D:\\themes"
#define DARKDASH_THEME_ROOT "D:\\themes\\default"

/* top-level screens */
#define SCR_MAIN    0
#define SCR_LAUNCH  1   /* shared app browser (Applications/Games/Homebrew/Emulators) */
#define SCR_FILEMAN 2   /* file manager */
#define SCR_SETTINGS 3  /* settings */
#define SCR_SAVEMGR 4   /* savegame manager */

/* orb glitch transition length, milliseconds */
#define TRANS_MS 400            /* = RECT_OUT_MS + SWING_OUT_MS: screen cuts when
                                   the door reaches edge-on (start of dwell), so
                                   the cut hides behind the flat/invisible panel */

                                   /* idle animation: after this long with no input, the orb occasionally glitches
                                      on its own. Each burst lasts IDLE_GLITCH_MS; the gap between bursts is
                                      randomised in [IDLE_GAP_MIN, IDLE_GAP_MIN+IDLE_GAP_RAND). */
#define IDLE_MS        8000
#define IDLE_GLITCH_MS 300
#define IDLE_GAP_MIN   4000
#define IDLE_GAP_RAND  7000

#define MENU_COUNT 7
static const char* k_menu[MENU_COUNT] = {
    "APPLICATIONS", "GAMES", "HOMEBREW", "EMULATORS", "FILE MANAGER", "SAVE MANAGER", "SETTINGS"
};

/* which LauncherConfig a menu row opens, or NULL if it isn't a browser row */
static const LauncherConfig* MenuConfig(int sel) {
    switch (sel) {
    case 0: return App_Config();
    case 1: return Games_Config();
    case 2: return Homebrew_Config();
    case 3: return Emulators_Config();
    default: return 0;   /* FILE MANAGER / SETTINGS handled separately */
    }
}

/* ambient bloom now lives in dd_backdrop (shared across all screens) */

/* --- header status ticker: temp / fan / IP / free space ---------------- */
static char  s_status[192] = "DARKDASH";
static DWORD s_lastStatus = 0;
static int   s_ntpDone = 0;    /* one-shot internet time sync at boot */

static void StrAppend(char* dst, int cap, const char* s) {
    strncat(dst, s, cap - (int)strlen(dst) - 1);
}
static void IntAppend(char* dst, int cap, int v) {
    char rev[12], num[12];
    int  n = 0, i = 0;
    if (v < 0) { StrAppend(dst, cap, "-"); v = -v; }
    if (v == 0) { StrAppend(dst, cap, "0"); return; }
    while (v > 0 && n < 11) { rev[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && i < 11) num[i++] = rev[--n];
    num[i] = 0;
    StrAppend(dst, cap, num);
}

/* poll sensors + net and rebuild the scroll string (called ~1/sec) */
static void BuildStatus(void) {
    int  cpu = 0, board = 0, fan = 0;

    s_status[0] = 0;
    if (Sys_ReadTemps(&cpu, &board)) {
        StrAppend(s_status, sizeof(s_status), "CPU ");
        IntAppend(s_status, sizeof(s_status), cpu);
        StrAppend(s_status, sizeof(s_status), "C    BOARD ");
        IntAppend(s_status, sizeof(s_status), board);
        StrAppend(s_status, sizeof(s_status), "C    ");
    }
    if (Sys_ReadFanPct(&fan)) {
        StrAppend(s_status, sizeof(s_status), "FAN ");
        IntAppend(s_status, sizeof(s_status), fan);
        StrAppend(s_status, sizeof(s_status), "%    ");
    }
    StrAppend(s_status, sizeof(s_status), "IP ");
    StrAppend(s_status, sizeof(s_status), Net_Ip());
    StrAppend(s_status, sizeof(s_status), "    ");
    {
        static const char* const drv[3] = { "E:\\", "F:\\", "G:\\" };
        static const char* const lbl[3] = { "E: ",  "F: ",  "G: " };
        int di;
        for (di = 0; di < 3; di++) {
            char disk[16];
            Sys_DiskFreeStr(drv[di], disk, sizeof(disk));
            if (disk[0] == 0 || disk[0] == '-') continue;   /* drive not present */
            StrAppend(s_status, sizeof(s_status), lbl[di]);
            StrAppend(s_status, sizeof(s_status), disk);
            StrAppend(s_status, sizeof(s_status), " FREE   ");
        }
    }
    StrAppend(s_status, sizeof(s_status), "    ");   /* gap before wrap */

    if (s_status[0] == 0) StrAppend(s_status, sizeof(s_status), "DARKDASH        ");
}

/* scroll s_status right-to-left inside the header bar, clipped to it */
static void DrawStatusScroller(IDirect3DDevice8* d) {
    static DWORD s_lastMs = 0;
    static float s_pos = 0.0f;   /* pixels scrolled so far, wraps at tw */
    DWORD ms = GetTickCount();
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    int   tw = Font_MeasureText(s_status, FONT_SIZE_SMALL);
    float baseX;
    DWORD dt;
    D3DVIEWPORT8 vpFull, vpClip;

    if (tw < 1) tw = 1;

    /* advance by real elapsed time -> continuous sub-pixel motion (smooth),
       and independent of tw, so the ~1/sec status rebuild can't teleport it */
    if (s_lastMs == 0) s_lastMs = ms;
    dt = ms - s_lastMs;
    s_lastMs = ms;
    if (dt > 200) dt = 200;                 /* clamp after a stall (e.g. a launch) */
    s_pos += (float)dt * 0.045f;            /* 45 px/sec */
    while (s_pos >= (float)tw) s_pos -= (float)tw;
    if (s_pos < 0.0f) s_pos = 0.0f;

    baseX = 16.0f - s_pos;

    d->GetViewport(&vpFull);
    vpClip.X = (DWORD)UI_Sx(16.0f); vpClip.Y = (DWORD)UI_Sy(14.0f);
    vpClip.Width = (DWORD)UI_ScaleX(286.0f); vpClip.Height = (DWORD)UI_ScaleY(28.0f);
    vpClip.MinZ = 0.0f; vpClip.MaxZ = 1.0f;
    d->SetViewport(&vpClip);
    /* two copies, one trailing, for a seamless wrap */
    Font_DrawText(d, baseX, 20.0f, s_status, FONT_SIZE_SMALL, text, 0);
    Font_DrawText(d, baseX + (float)tw, 20.0f, s_status, FONT_SIZE_SMALL, text, 0);
    d->SetViewport(&vpFull);
}

/* Small "recently launched" overlay (Y on the main menu). Names only, no art;
   uses the themed menu frame, same treatment as the FileMan operation popups. */
static void DrawRecents(int selRow) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* frame = Theme_Asset("frame_menu_v");
    float bx = 200.0f, by = 120.0f, bw = 240.0f, bh = 240.0f;
    int   rc = Recents_Count();
    int   i;

    /* dim the scene, then the themed frame panel (matches FileMan dialogs) */
    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(150, 0, 0, 0));
    if (frame) UI_DrawSprite(frame, bx, by, bw, bh, 0xFFFFFFFF, 0);
    else       UI_FillRect(bx, by, bw, bh, UI_ARGB(235, 18, 22, 18));

    Font_DrawText(d, bx + 20.0f, by + 16.0f, "RECENT", FONT_SIZE_SMALL, accent, 0);

    if (rc == 0) {
        Font_DrawText(d, bx + 20.0f, by + 56.0f, "Nothing launched yet", FONT_SIZE_SMALL, dim, (int)(bw - 40.0f));
    }
    else {
        for (i = 0; i < rc; i++) {
            float ry = by + 48.0f + (float)i * 30.0f;
            if (i == selRow)
                UI_FillRect(bx + 12.0f, ry - 2.0f, bw - 24.0f, 26.0f, UI_ARGB(90, ar, ag, ab));
            Font_DrawText(d, bx + 22.0f, ry, Recents_Name(i), FONT_SIZE_SMALL,
                (i == selRow) ? glow : text, (int)(bw - 44.0f));
        }
    }
    Font_DrawText(d, bx + 20.0f, by + bh - 30.0f, "A LAUNCH   Y/B CLOSE", FONT_SIZE_SMALL, dim, 0);
}

/* Power menu overlay (WHITE tap on the main menu). Themed frame, same treatment
   as the recents overlay. Three options: Restart / Reboot / Shutdown. */
static void DrawPowerMenu(int selRow) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* frame = Theme_Asset("frame_menu_v");
    static const char* const k_pow[3] = { "Restart", "Reboot", "Shutdown" };
    float bx = 232.0f, by = 150.0f, bw = 176.0f, bh = 158.0f;
    int   i;

    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(150, 0, 0, 0));
    if (frame) UI_DrawSprite(frame, bx, by, bw, bh, 0xFFFFFFFF, 0);
    else       UI_FillRect(bx, by, bw, bh, UI_ARGB(235, 18, 22, 18));

    Font_DrawText(d, bx + 20.0f, by + 16.0f, "POWER", FONT_SIZE_SMALL, accent, 0);
    for (i = 0; i < 3; i++) {
        float ry = by + 46.0f + (float)i * 28.0f;
        if (i == selRow)
            UI_FillRect(bx + 12.0f, ry - 2.0f, bw - 24.0f, 24.0f, UI_ARGB(90, ar, ag, ab));
        Font_DrawText(d, bx + 22.0f, ry, k_pow[i], FONT_SIZE_SMALL,
            (i == selRow) ? glow : text, 0);
    }
    Font_DrawText(d, bx + 20.0f, by + bh - 26.0f, "A SELECT   B CLOSE", FONT_SIZE_SMALL, dim, 0);
}

static void DrawSplash(int sel, int glowAlpha, int glitch) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD glow = Theme_GlowColor();      /* [glow] color, falls back to palette.glow */
    int   glowOn = Theme_GlowEnabled();
    int   glowI = Theme_GlowIntensity();  /* 0..100 */
    const Texture* orb = Theme_Asset("orb_hero");
    const Texture* hdr = Theme_Asset("bar_header");
    const Texture* menu = Theme_Asset("frame_menu_v");
    const Texture* foot = Theme_Asset("bar_footer");
    int   ar = (int)((accent >> 16) & 0xFF);
    int   ag = (int)((accent >> 8) & 0xFF);
    int   ab = (int)(accent & 0xFF);
    int   gr = (int)((glow >> 16) & 0xFF);
    int   gg = (int)((glow >> 8) & 0xFF);
    int   gb = (int)(glow & 0xFF);
    float menuX = 352.0f, menuY = 48.0f;
    /* 7 rows centred inside the frame interior. Pitch adapts to the font's line
       height so a tall custom font doesn't overlap rows, but is capped so all 7
       still fit inside the panel interior (no spill past the frame). */
    float rowY0 = 112.0f, rowDY = 40.0f;
    int i;
    {
        float lh = (float)Font_LineHeight(FONT_SIZE_MEDIUM);
        float interiorB = menuY + 384.0f - 24.0f;      /* panel bottom, less padding */
        float maxPitch = (interiorB - rowY0) / 7.0f;  /* pitch that fits 7 rows */
        if (rowDY < lh)       rowDY = lh;              /* don't let rows touch */
        if (rowDY > maxPitch) rowDY = maxPitch;        /* don't spill the frame */
    }

    /* --- ambient theme lighting: shared green bloom (same on every screen) --- */
    Backdrop_Draw();

    /* --- orb: FLAT (baked iso). Normal = breathe pulse; during a screen
       transition it glitches with a chromatic RGB split + jitter + flicker.
       All integer math (LCG); only int->float in the draw call, no Ftoi.
       Easter egg: when active, the spinning Darkone83 logo replaces the orb. --- */
    if (Egg_Active()) {
        if (glitch > 0 && orb) {
            /* brief chromatic flash on the orb sprite as the logo swaps in */
            DWORD r = GetTickCount() * 1664525u + 1013904223u;
            int   amp = 3 + glitch / 4, jx, jy, fa;
            jx = ((int)((r >> 26) & 0x1F) - 16) * amp / 10;
            r = r * 1664525u + 1013904223u;
            jy = ((int)((r >> 26) & 0x1F) - 16) * amp / 18;
            r = r * 1664525u + 1013904223u;
            fa = 100 + (int)((r >> 24) & 0x8F);
            UI_DrawSprite(orb, 44.0f - (float)jx, 168.0f + (float)jy, 272.0f, 234.0f, UI_ARGB(fa, 255, 0, 0), 1);
            UI_DrawSprite(orb, 44.0f + (float)jx, 168.0f - (float)jy, 272.0f, 234.0f, UI_ARGB(fa, 0, 0, 255), 1);
        }
        Egg_Draw(44, 168, 272, 234);   /* spinning logo quad over the orb region */
    }
    else if (orb) {
        if (glitch > 0) {
            DWORD r = GetTickCount() * 1664525u + 1013904223u;
            int   amp = 3 + glitch / 4;                  /* ~3..28 px swing */
            int   jx, jy, fa, tear;
            jx = ((int)((r >> 26) & 0x1F) - 16) * amp / 10;
            r = r * 1664525u + 1013904223u;
            jy = ((int)((r >> 26) & 0x1F) - 16) * amp / 18;
            r = r * 1664525u + 1013904223u;
            fa = 100 + (int)((r >> 24) & 0x8F);          /* deeper flicker 100..243 */
            r = r * 1664525u + 1013904223u;
            tear = ((int)((r >> 25) & 0x3F) - 32) * amp / 28;  /* horizontal channel tear */
            /* dim ghost so the orb never fully drops out */
            UI_DrawSprite(orb, 44.0f, 168.0f, 272.0f, 234.0f, UI_ARGB(50, 255, 255, 255), 0);
            /* RGB channels, additive, offset + torn against each other */
            UI_DrawSprite(orb, 44.0f - (float)jx + (float)tear, 168.0f + (float)jy, 272.0f, 234.0f, UI_ARGB(fa, 255, 0, 0), 1);
            UI_DrawSprite(orb, 44.0f, 168.0f, 272.0f, 234.0f, UI_ARGB(fa, 0, 255, 0), 1);
            UI_DrawSprite(orb, 44.0f + (float)jx - (float)tear, 168.0f - (float)jy, 272.0f, 234.0f, UI_ARGB(fa, 0, 0, 255), 1);
        }
        else {
            UI_DrawSprite(orb, 44.0f, 168.0f, 272.0f, 234.0f, 0xFFFFFFFF, 0);
            UI_DrawSprite(orb, 44.0f, 168.0f, 272.0f, 234.0f,
                UI_ARGB(glowAlpha, 255, 255, 255), 1);
            /* rare specular shimmer: the orb briefly catches light every ~38s.
               Re-draw the orb additively with the glow tint, ramping up then
               down (~900ms) -- respects the orb's shape (it IS the orb sprite)
               and reads as a glint rather than a flat flash. Gated by DD_FX_IDLE. */
            if (Data_FxOn(DD_FX_IDLE)) {
                DWORD st = GetTickCount();
                DWORD cyc = st % 38000;            /* one shimmer per ~38s */
                if (cyc < 900) {
                    float p = (float)cyc / 900.0f;            /* 0..1         */
                    float hump = 4.0f * p * (1.0f - p);        /* 0->1->0      */
                    int   a = (int)(120.0f * hump);
                    if (a > 0)
                        UI_DrawSprite(orb, 44.0f, 168.0f, 272.0f, 234.0f,
                            UI_ARGB(a, gr, gg, gb), 1);
                }
            }
        }
    }

    /* --- flat top-left header bar: frame + live status ticker --- */
    if (hdr) UI_DrawSprite(hdr, 8.0f, 8.0f, 300.0f, 40.0f, 0xFFFFFFFF, 0);
    DrawStatusScroller(d);

    /* --- disc popup (top-right): shown only when a game disc is present --- */
    {
        const DiscState* ds = Disc_Get();
        if (ds->present && ds->isXboxGame) {
            const Texture* dbar = Theme_Asset("bar_header");
            float bx = 380.0f, by = 8.0f, bw = 252.0f, bh = 40.0f;
            float gh = (float)Font_GlyphHeight(FONT_SIZE_SMALL);
            float ty = by + (bh - gh) * 0.5f;        /* vertically centre in the bar */
            if (ty < by + 2.0f) ty = by + 2.0f;
            if (dbar) UI_DrawSprite(dbar, bx, by, bw, bh, 0xFFFFFFFF, 0);
            /* single line to match the one-line header frame: action label in the
               accent glow, then the title (clipped to the bar width). */
            Font_DrawText(d, bx + 12.0f, ty, "START:", FONT_SIZE_SMALL, glow, 0);
            Font_DrawText(d, bx + 64.0f, ty, ds->title, FONT_SIZE_SMALL, dim, (int)(bw - 76.0f));
        }
    }

    /* tilted console. The selection rect is animated SEPARATELY from the door:
       it fades out before the swing, stays hidden while the door (frame+text)
       turns, and fades back in once the door closes -- so the highlight never
       has to be transformed onto the 3D quad. */
    {
        int turning = Swing_Doorturning();   /* door actually rotating?     */
        int rectA = Swing_RectAlpha();      /* 255 rest .. 0 during swing  */
        int captured = 0;

        if (turning)
            captured = Swing_CaptureBegin(menuX, menuY, 272.0f, 384.0f);

        /* --- frame + text (the part that swings). No rect here -- ever. --- */
        Iso_Begin();
        if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
        for (i = 0; i < MENU_COUNT; i++) {
            DWORD c = (i == sel) ? glow : text;
            Font_DrawTextIso(d, menuX + 30.0f, rowY0 + rowDY * (float)i + 4.0f,
                k_menu[i], FONT_SIZE_MEDIUM, c);
        }
        Iso_End();

        if (captured) {
            /* frame+text captured -> restore backbuffer -> swing the quad */
            Swing_CaptureEnd();
            Swing_DrawCaptured(menuX, menuY, 272.0f, 384.0f);
        }
        else if (rectA > 0 && glowOn) {
            /* not turning (at rest, or a rect-fade phase): draw the highlight
               flat over the console at its current fade level. Honors the
               theme [glow] block (enable + intensity + dedicated color).
               The Y eases toward the selected row with a pop + chromatic tick. */
            float gy = rowY0 + rowDY * (float)sel - 6.0f;
            int   ha = (70 + glowAlpha) * rectA / 255;
            ha = ha * glowI / 100;
            Iso_Begin();
            Select_Begin(0x1000, gy);
            if (ha > 0)
                Select_DrawGlow(menuX + 18.0f, gy, 210.0f, 36.0f,
                    UI_ARGB(ha, gr, gg, gb));
            Iso_End();
        }
    }

    /* --- footer status bar: FLAT --- */
    if (foot) UI_DrawSprite(foot, 8.0f, 442.0f, 624.0f, 32.0f, 0xFFFFFFFF, 0);
    {
        /* vertically center the hint in the 442..474 bar using the *measured*
           glyph height, so a tall custom font doesn't clip at the bottom */
        float barTop = 442.0f, barH = 32.0f;
        float gh = (float)Font_GlyphHeight(FONT_SIZE_SMALL);
        float ty = barTop + (barH - gh) * 0.5f;
        if (ty < barTop + 2.0f) ty = barTop + 2.0f;
        Font_DrawText(d, 24.0f, ty,
            "A SELECT   B BACK   START MENU", FONT_SIZE_SMALL, text, 0);
    }
}

void __cdecl main(void) {
    int   sel = 0;
    WORD  prev = 0;
    int   running = 1;
    int   screen = SCR_MAIN;
    int   trans = 0;            /* 0 none | 1 glitch-out | 2 glitch-in */
    DWORD transStart = 0;
    int   transTarget = SCR_MAIN;
    const LauncherConfig* pendingCfg = 0;   /* config to open when SCR_LAUNCH lands */
    DWORD lastInputMs = GetTickCount();   /* for the idle animation              */
    DWORD idleGlitchAt = 0;                /* when the current idle glitch began (0=none) */
    DWORD nextIdleAt = 0;                /* earliest time the next idle glitch may fire */
    DWORD idleSeed = GetTickCount() | 1;
    int   recOpen = 0;          /* recents overlay (Y on main menu) */
    int   recSel = 0;
    int   powOpen = 0;          /* power menu overlay (WHITE tap) */
    int   powSel = 0;
    int   saverOn = 0;          /* screensaver active */
    DWORD whiteDownMs = 0;      /* when WHITE went down (0 = not down) */
    int   whiteMoved = 0;      /* DPAD touched during this WHITE hold? */

    /* Point D: at our own install folder BEFORE anything reads "D:\...". When
       booted as the dashboard (Cerbios) the kernel's D: may be absent/wrong, so
       without this every asset/theme/data path fails to resolve. */
    Mount_SelfToD();
    Data_Load();             /* load prefs first: Gfx_Init reads videoRes from it */
    Recents_Init();          /* most-recently-launched titles (Y overlay) */
    Paths_Load();            /* user-added custom scan paths per category */
    Time_Load();             /* NTP enable + timezone prefs (time.dat)        */
    if (!Gfx_Init()) return;
    UI_Init(Gfx_Width(), Gfx_Height());
    Calib_Apply();           /* apply saved overscan insets (zero if uncalibrated) */
    Font_Init(Gfx_Device());
    InitInput();
    Mount_HddPartitions();   /* map C/E/F/G before anything scans them (X/Y/Z cache skipped) */

    /* apply the saved custom font, if any. Font_Init already loaded the baked
       Default, so a missing/bad file just leaves Default in place. */
    {
        DD_Settings* st = Data_Get();
        if (st->fontName[0]) {
            char path[160];
            lstrcpyA(path, "D:\\fonts\\");
            lstrcatA(path, st->fontName);
            lstrcatA(path, ".ddf");
            Font_LoadDDF(Gfx_Device(), path);   /* failure -> stays on Default */
        }
    }
    Sys_Init();              /* clear any stuck SMBus before sensor polling   */
    Disc_Init();             /* optical disc monitor (mounts a game disc to Q:) */
    Net_Start();             /* bring the network stack up, kick DHCP         */
    UI_SetStretch(Data_Get()->videoAspect == DD_VIDEO_STRETCH);  /* apply saved aspect */
    {
        DD_Settings* st = Data_Get();
        int loaded = 0;
        if (st->themeName[0]) {
            char root[160];
            Theme_RootFor(DARKDASH_THEMES_DIR, st->themeName, root, sizeof(root));
            loaded = Theme_Load(root);
        }
        if (!loaded) Theme_Load(DARKDASH_THEME_ROOT);   /* default failsafe */
    }
    Backdrop_Init();
    Fx_Init();               /* CRT scanline texture + overlay effects */
    Audio_Init();
    {
        DD_Settings* st = Data_Get();
        if (st->musicCustom && st->musicPath[0]) Audio_SetMusicPath(st->musicPath);
        Audio_SetMusicVolume(st->musicVolume);   /* before play, so it starts at level */
    }
    Audio_StartMusic(1);   /* loop bg.mp3 (or the custom track) */

    /* FTP service: mark it wanted if enabled -- it will bring itself up once
       the network/DHCP is actually ready (Net_IsUp is false this early at boot,
       so we can't start it here; Ftp_Tick handles the deferred start). */
    Ftp_Init();
    if (Data_Get()->ftpEnabled)
        Ftp_Want(1);

    Lcd_Init();              /* physical LCD accessory: load lcd.dat, probe, splash */
    Udp_Init();              /* shared UDP layer for DarkoneCustoms accessories */
    TypeD_Init();            /* Type-D status broadcaster (UDP 50504)           */
    Oxfp_Init();             /* OXFP front-panel control (UDP 32123)            */
    Rgb_Init();              /* XBOX-RGB control (UDP 7777)                     */

    /* first boot: run screen calibration so the user can fix overscan up front.
       Everything it needs (font, audio, backdrop) is initialised by now. */
    if (Calib_NeedsRun()) {
        Calib_Run();
        /* The A/B that exited calibration may still be held as we enter the main
           loop. Seed 'prev' with the current button state so that release isn't
           read as a fresh press (which would instantly select a menu row). */
        PumpInput();
        prev = GetButtons();
    }

    /* Display settle: emulators show output instantly, but a real LCD/HDTV
       needs time to lock onto the signal after the video mode is set -- during
       which the boot fade would already be playing (and missed). Present ~0.7s
       of blank frames here so the panel acquires sync on real output before the
       fade starts, making the power-on flourish actually visible on hardware. */
    {
        DWORD settleStart = GetTickCount();
        while (GetTickCount() - settleStart < 700) {
            Gfx_BeginFrame(Theme_BG());
            Gfx_EndFrame();
        }
    }

    Fx_BootBegin();          /* power-on flourish on the first frames */

    while (running) {
        WORD btn, pressed;
        int  whiteReleased;
        DWORD t, el;
        int phase, tri, glowA, tuning, glitch;

        PumpInput();
        btn = GetButtons();

        Ftp_Tick();   /* service the FTP server every frame (single-threaded) */
        Lcd_Tick();   /* refresh the physical LCD accessory (background service) */
        TypeD_Tick(); /* broadcast Type-D status (background, rate-limited)       */
        Udp_DiscoTick(); /* discover XBOX-RGB / OXFP for accessory menu gating    */
        pressed = (WORD)(btn & ~prev);
        whiteReleased = ((prev & BTN_WHITE) && !(btn & BTN_WHITE)) ? 1 : 0;
        prev = btn;
        if (btn) lastInputMs = GetTickCount();   /* any input cancels idle */

        /* screensaver: any input drops out of it and is swallowed (so the wake
           press doesn't also navigate the menu underneath). */
        if (saverOn) {
            if (btn || pressed) {
                Saver_Exit();
                saverOn = 0;
                pressed = 0;
                whiteReleased = 0;
            }
        }

        tuning = (btn & BTN_WHITE) ? 1 : 0;

        /* WHITE double-duty: HOLD (with DPAD) = iso angle-tuning egg; a quick
           TAP (no DPAD, released fast) = open the power menu. Track the hold. */
        if (pressed & BTN_WHITE) { whiteDownMs = GetTickCount(); whiteMoved = 0; }
        if ((btn & BTN_WHITE) && (pressed & (BTN_DPAD_UP | BTN_DPAD_DOWN | BTN_DPAD_LEFT | BTN_DPAD_RIGHT)))
            whiteMoved = 1;
        if (whiteReleased) {   /* WHITE released this frame */
            DWORD held = GetTickCount() - whiteDownMs;
            if (whiteDownMs && held < 400 && !whiteMoved &&
                screen == SCR_MAIN && trans == 0 && !recOpen && !powOpen) {
                powSel = 0; powOpen = 1; Audio_PlaySfx(SFX_SELECT);
            }
            whiteDownMs = 0;
        }
        if (trans != 0) {
            /* transition animating -- swallow menu input until it settles */
        }
        else if (screen == SCR_LAUNCH) {
            if (Launcher_Update(pressed, btn)) {     /* B -> back to main */
                screen = SCR_MAIN;
                trans = 2; transStart = GetTickCount();   /* glitch the orb in */
                Swing_StartIn();                          /* main menu swings in */
            }
        }
        else if (screen == SCR_FILEMAN) {
            if (FileMan_Update(pressed, btn)) {      /* B -> back to main */
                screen = SCR_MAIN;
                trans = 2; transStart = GetTickCount();
                Swing_StartIn();
            }
        }
        else if (screen == SCR_SETTINGS) {
            if (Settings_Update(pressed, btn)) {     /* B -> back to main */
                screen = SCR_MAIN;
                trans = 2; transStart = GetTickCount();
                Swing_StartIn();
            }
        }
        else if (screen == SCR_SAVEMGR) {
            if (SaveMgr_Update(pressed, btn)) {      /* B -> back to main */
                screen = SCR_MAIN;
                trans = 2; transStart = GetTickCount();
                Swing_StartIn();
            }
        }
        else if (tuning) {
            if (btn & BTN_DPAD_UP)    Iso_NudgeAngles(0.6f, 0.0f);
            if (btn & BTN_DPAD_DOWN)  Iso_NudgeAngles(-0.6f, 0.0f);
            if (btn & BTN_DPAD_LEFT)  Iso_NudgeAngles(0.0f, -0.6f);
            if (btn & BTN_DPAD_RIGHT) Iso_NudgeAngles(0.0f, 0.6f);
        }
        else {
            if (powOpen) {
                /* power menu owns input while open */
                if (pressed & BTN_DPAD_DOWN) { if (powSel < 2) { powSel++; Audio_PlaySfx(SFX_NAV_DOWN); } }
                if (pressed & BTN_DPAD_UP) { if (powSel > 0) { powSel--; Audio_PlaySfx(SFX_NAV_UP); } }
                if (pressed & BTN_A) {
                    Audio_PlaySfx(SFX_SELECT);
                    /* tidy shutdown of services before we hand off / power down */
                    Ftp_Stop();
                    Audio_StopMusic();
                    if (powSel == 0) {                 /* Restart: relaunch DarkDash */
                        LAUNCH_DATA ld; ZeroMemory(&ld, sizeof(ld));
                        XLaunchNewImage("D:\\default.xbe", &ld);
                    }
                    else if (powSel == 1) {          /* Reboot: warm reset (0x01) */
                        Sys_Reset();
                    }
                    else {                           /* Shutdown: power off */
                        Sys_PowerOff();
                    }
                    /* if a relaunch somehow returns, just close the menu */
                    powOpen = 0;
                }
                if (pressed & BTN_B) { powOpen = 0; Audio_PlaySfx(SFX_BACK); }
            }
            else if (recOpen) {
                /* recents overlay owns input while open */
                int rc = Recents_Count();
                if (pressed & BTN_DPAD_DOWN) { if (recSel < rc - 1) { recSel++; Audio_PlaySfx(SFX_NAV_DOWN); } }
                if (pressed & BTN_DPAD_UP) { if (recSel > 0) { recSel--; Audio_PlaySfx(SFX_NAV_UP); } }
                if ((pressed & BTN_A) && rc > 0) {
                    const char* path = Recents_Path(recSel);
                    Audio_PlaySfx(SFX_SELECT);
                    if (path && path[0]) {
                        Audio_StopMusic();
                        Mount_LaunchXbe(path);   /* no return on success */
                        /* fell through -> launch failed; close and carry on */
                    }
                    recOpen = 0;
                }
                if (pressed & (BTN_B | BTN_Y)) { recOpen = 0; Audio_PlaySfx(SFX_BACK); }
            }
            else {
                if (pressed & BTN_DPAD_DOWN) { if (sel < MENU_COUNT - 1) { sel++; Audio_PlaySfx(SFX_NAV_DOWN); } }
                if (pressed & BTN_DPAD_UP) { if (sel > 0) { sel--; Audio_PlaySfx(SFX_NAV_UP); } }
                if (pressed & BTN_A) {
                    const LauncherConfig* cfg = MenuConfig(sel);
                    Audio_PlaySfx(SFX_SELECT);
                    Fx_FlashEdge();                      /* edge-glow pulse on select */
                    if (cfg) {                           /* a browser row: glitch out, then open */
                        pendingCfg = cfg;
                        trans = 1; transStart = GetTickCount(); transTarget = SCR_LAUNCH;
                        Swing_Start();                       /* door-swing the menu out */
                    }
                    else if (sel == 4) {               /* FILE MANAGER */
                        trans = 1; transStart = GetTickCount(); transTarget = SCR_FILEMAN;
                        Swing_Start();
                    }
                    else if (sel == 5) {               /* SAVE MANAGER */
                        trans = 1; transStart = GetTickCount(); transTarget = SCR_SAVEMGR;
                        Swing_Start();
                    }
                    else if (sel == 6) {               /* SETTINGS */
                        trans = 1; transStart = GetTickCount(); transTarget = SCR_SETTINGS;
                        Swing_Start();
                    }
                    /* (no rows past SETTINGS) */
                }
                if (pressed & BTN_X) Audio_PlaySfx(SFX_ALT);
                if (pressed & BTN_B) Audio_PlaySfx(SFX_BACK);
                if (pressed & BTN_START) {           /* launch a present game disc */
                    const DiscState* ds = Disc_Get();
                    if (ds->present && ds->isXboxGame) {
                        Audio_PlaySfx(SFX_SELECT);
                        Disc_Launch();               /* no return on success */
                    }
                }
                if (pressed & BTN_Y) {               /* open the recents overlay */
                    recSel = 0;
                    recOpen = 1;
                    Audio_PlaySfx(SFX_SELECT);
                }
                /* easter egg: Black+RT toggles the spinning Darkone83 logo on the
                   pedestal. Fires once when the combo completes; glitches the swap. */
                if ((btn & BTN_BLACK) && (btn & BTN_RTRIG) &&
                    (pressed & (BTN_BLACK | BTN_RTRIG))) {
                    Egg_Toggle();
                    trans = 2; transStart = GetTickCount();   /* glitch the swap in */
                    Audio_PlaySfx(SFX_ALT);
                }
            }   /* end !recOpen */
        }

        if ((btn & BTN_LTRIG) && (btn & BTN_RTRIG) && (btn & BTN_BACK))
            running = 0;

        /* integer breathe pulse (no float->int; avoids Ftoi on MSVC2003) */
        t = GetTickCount();
        phase = (int)((t >> 3) & 255);
        tri = (phase < 128) ? phase : (255 - phase);
        glowA = 24 + (tri >> 2);

        /* idle life: slow ambient sway of the whole iso stage so the screen
           breathes even at rest. Subtle (~+/-0.8 pitch, +/-1.2 yaw) on long
           coprime periods so it never visibly loops. Added on top of the tuned
           angles, so the WHITE+DPAD tuning egg still works. Gated by DD_FX_IDLE. */
        if (Data_FxOn(DD_FX_IDLE)) {
            float ap = (float)(t % 19000) / 19000.0f * 6.2831853f;  /* ~19s */
            float ay = (float)(t % 27000) / 27000.0f * 6.2831853f;  /* ~27s */
            Iso_SetBreathe(0.8f * (float)sin((double)ap),
                1.2f * (float)sin((double)ay));
        }
        else {
            Iso_SetBreathe(0.0f, 0.0f);
        }

        /* advance the menu door-swing (real per-frame delta, clamped) */
        {
            static DWORD s_swingLast = 0;
            DWORD sdt;
            if (s_swingLast == 0) s_swingLast = t;
            sdt = t - s_swingLast;
            s_swingLast = t;
            if (sdt > 100) sdt = 100;
            Swing_Update(sdt);
        }

        /* refresh header status (~1/sec): sensors + IP + disc tray */
        if (s_lastStatus == 0 || t - s_lastStatus >= 1000) {
            s_lastStatus = t;
            Net_Poll();
            BuildStatus();
            Disc_Poll();          /* mount/unmount a disc on tray change */

            /* one-shot internet time sync: fire once DHCP has resolved an
               address, if the user enabled it. Give up after a grace window so
               we don't keep retrying a dead/timeserver-less network forever. */
            if (!s_ntpDone && Time_NtpEnabled()) {
                if (Net_IsUp()) {
                    Ntp_Sync();        /* best-effort; failure is silent at boot */
                    s_ntpDone = 1;
                }
                else if (t > 30000) { /* ~30s and still no link/lease -> stop */
                    s_ntpDone = 1;
                }
            }
        }

        /* transition timer -> orb glitch intensity (0..100) */
        glitch = 0;
        if (trans == 1) {                 /* glitch OUT, then cut to target */
            el = t - transStart;
            glitch = (int)(el * 100 / TRANS_MS);
            if (el >= TRANS_MS) {
                screen = transTarget;
                if (transTarget == SCR_LAUNCH)  Launcher_Enter(pendingCfg);
                else if (transTarget == SCR_FILEMAN) FileMan_Enter();
                else if (transTarget == SCR_SETTINGS) Settings_Enter();
                else if (transTarget == SCR_SAVEMGR) SaveMgr_Enter();
                trans = 0; glitch = 0;
            }
        }
        else if (trans == 2) {          /* glitch IN, settling on main */
            el = t - transStart;
            glitch = 100 - (int)(el * 100 / TRANS_MS);
            if (el >= TRANS_MS) { trans = 0; glitch = 0; }
        }
        if (glitch < 0)   glitch = 0;
        if (glitch > 100) glitch = 100;

        /* idle animation: on the main menu, with no transition and no input
           for a while, fire an occasional self-glitch. It only writes 'glitch'
           (no screen change), so navigation stays live throughout. */
        if (screen == SCR_MAIN && trans == 0) {
            if (idleGlitchAt != 0) {
                DWORD ge = t - idleGlitchAt;
                if (ge >= IDLE_GLITCH_MS) {
                    idleGlitchAt = 0;
                    idleSeed = idleSeed * 1664525u + 1013904223u;
                    nextIdleAt = t + IDLE_GAP_MIN + (idleSeed >> 18) % IDLE_GAP_RAND;
                }
                else {
                    int half = IDLE_GLITCH_MS / 2;
                    int g = (ge < (DWORD)half)
                        ? (int)(ge * 100 / half)
                        : (int)((IDLE_GLITCH_MS - ge) * 100 / half);
                    if (g > glitch) glitch = g;     /* triangle ramp 0->100->0 */
                }
            }
            else if ((t - lastInputMs) > IDLE_MS && t >= nextIdleAt) {
                idleGlitchAt = t;                   /* begin a burst */
            }
        }
        else {
            idleGlitchAt = 0;                       /* leaving main cancels it */
        }

        /* screensaver: engage after the configured idle timeout, only on the
           bare main menu (no transition, no overlay). 0 = disabled. */
        if (!saverOn) {
            int ssMin = Data_Get()->screensaverMin;
            if (ssMin > 0 && screen == SCR_MAIN && trans == 0 && !recOpen && !powOpen &&
                (t - lastInputMs) > (DWORD)ssMin * 60000u) {
                Saver_Enter();
                saverOn = 1;
            }
        }
        if (saverOn) Saver_Update();

        Audio_Update();   /* service Xbox DS mixer every frame */
        Gfx_BeginFrame(Theme_BG());
        if (saverOn) {
            Saver_Render();
        }
        else {
            if (screen == SCR_LAUNCH)       Launcher_Render();
            else if (screen == SCR_FILEMAN) FileMan_Render();
            else if (screen == SCR_SETTINGS) Settings_Render();
            else if (screen == SCR_SAVEMGR) SaveMgr_Render();
            else                            DrawSplash(sel, glowA, glitch);
            if (screen == SCR_MAIN && recOpen) DrawRecents(recSel);
            if (screen == SCR_MAIN && powOpen) DrawPowerMenu(powSel);
        }
        /* ambient overlays, over all content: CRT scanlines + roll, then the
           SFX-synced edge flash. The boot intro (if active) tops everything.
           Each gated by its DD_FX_* toggle. */
        if (Data_FxOn(DD_FX_SCANLINES)) Fx_DrawScanlines();
        if (Data_FxOn(DD_FX_EDGE))      Fx_DrawEdgeGlow();
        if (Fx_BootActive()) Fx_DrawBoot();
        Gfx_EndFrame();
    }

    Ftp_Stop();
    Fx_Shutdown();
    Egg_Shutdown();
    Audio_Shutdown();
    Backdrop_Shutdown();
    Theme_Unload();
    Font_Shutdown();
    Gfx_Shutdown();

    {
        LAUNCH_DATA ld;
        ZeroMemory(&ld, sizeof(ld));
        XLaunchNewImage(NULL, &ld);
    }
}