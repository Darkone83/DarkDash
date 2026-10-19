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
#include "dd_arcs.h"
#include "dd_theme.h"
#include "dd_plasma.h"
#include "dd_texture.h"
#include "font.h"
#include "dd_hints.h"
#include "input.h"
#include "dd_audio.h"
#include "dd_jukebox.h"
#include "dd_data.h"
#include "dd_backdrop.h"
#include "dd_swing.h"
#include "dd_mount.h"
#include "dd_storage.h"
#include "dd_disc.h"
#include "dd_egg.h"
#include "dd_calib.h"
#include "dd_watchdog.h"
#include "dd_smbsvc.h"
#include "dd_xview.h"
#include "dd_trace.h"
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
#include "dd_typedart.h"
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

/* A4 environmental depth is shared across every menu and dialog.
   DD_A4_DEPTH lives in dd_ui.h (or a project-wide compiler definition),
   so one switch consistently controls every translation unit. */

   /* P4 diagnostic overlay. Compile with DD_PERF_HUD=1 to profile on-console.
      0 compiles out ALL new timing/rendering work. No settings or input changes. */
#ifndef DD_PERF_HUD
#define DD_PERF_HUD 0
#endif
#if DD_PERF_HUD
#include <stdio.h>

      /* GetTickCount has 1-ms granularity: these numbers are diagnostics, not GPU
         timer queries. Render measures CPU-side draw submission; Present includes
         any VBlank wait. FTP cost is inclusive of any synchronous disk I/O. */
typedef struct {
    DWORD frames, elapsedMs, avgFrameMs10, peakFrameMs;
    DWORD spikeCount, avgFtpMs10, peakFtpMs;
    DWORD avgRenderMs10, peakRenderMs, avgPresentMs10, peakPresentMs;
    DWORD fps10;
} DD_PerfSnapshot;

static DD_PerfSnapshot s_perfSnapshot;
static DWORD s_perfWindowAt = 0;
static DWORD s_perfLastFrameAt = 0;
static DWORD s_perfFrames = 0;
static DWORD s_perfFrameTotal = 0, s_perfFramePeak = 0, s_perfSpikes = 0;
static DWORD s_perfFtpTotal = 0, s_perfFtpPeak = 0;
static DWORD s_perfDrawTotal = 0, s_perfDrawPeak = 0;
static DWORD s_perfPresentTotal = 0, s_perfPresentPeak = 0;

/* Only record complete frame intervals; an occasional slow asset decode is
   still visible as a spike but a large clock gap cannot overflow the sums. */
static void Perf_BeginLoop(void) {
    DWORD now = GetTickCount();
    if (!s_perfWindowAt) s_perfWindowAt = now;
    if (s_perfLastFrameAt) {
        DWORD dt = now - s_perfLastFrameAt;
        if (dt < 2000u) {
            s_perfFrameTotal += dt;
            if (dt > s_perfFramePeak) s_perfFramePeak = dt;
            if (dt > ((Gfx_VideoModeStr()[0] == '5') ? 25u : 20u)) ++s_perfSpikes;
            ++s_perfFrames;
        }
    }
    s_perfLastFrameAt = now;
    if ((DWORD)(now - s_perfWindowAt) >= 1000u && s_perfFrames) {
        DWORD elapsed = now - s_perfWindowAt;
        s_perfSnapshot.frames = s_perfFrames;
        s_perfSnapshot.elapsedMs = elapsed;
        s_perfSnapshot.fps10 = (s_perfFrames * 10000u) / elapsed;
        s_perfSnapshot.avgFrameMs10 = (s_perfFrameTotal * 10u) / s_perfFrames;
        s_perfSnapshot.peakFrameMs = s_perfFramePeak;
        s_perfSnapshot.spikeCount = s_perfSpikes;
        s_perfSnapshot.avgFtpMs10 = (s_perfFtpTotal * 10u) / s_perfFrames;
        s_perfSnapshot.peakFtpMs = s_perfFtpPeak;
        s_perfSnapshot.avgRenderMs10 = (s_perfDrawTotal * 10u) / s_perfFrames;
        s_perfSnapshot.peakRenderMs = s_perfDrawPeak;
        s_perfSnapshot.avgPresentMs10 = (s_perfPresentTotal * 10u) / s_perfFrames;
        s_perfSnapshot.peakPresentMs = s_perfPresentPeak;
        s_perfFrames = 0;
        s_perfFrameTotal = s_perfFramePeak = s_perfSpikes = 0;
        s_perfFtpTotal = s_perfFtpPeak = 0;
        s_perfDrawTotal = s_perfDrawPeak = 0;
        s_perfPresentTotal = s_perfPresentPeak = 0;
        s_perfWindowAt = now;
    }
}

static void Perf_RecordCost(DWORD cost, DWORD* total, DWORD* peak) {
    /* Ignore extreme pauses when summing so arithmetic stays bounded. */
    if (cost > 2000u) cost = 2000u;
    *total += cost;
    if (cost > *peak) *peak = cost;
}

static void Perf_DrawHud(void) {
    char line[112];
    IDirect3DDevice8* d = Gfx_Device();
    const DWORD txt = UI_ARGB(255, 238, 249, 240);
    DWORD s = s_perfSnapshot.fps10;
    if (!d || !s_perfSnapshot.frames) return;
    UI_FillRect(8.0f, 54.0f, 296.0f, 114.0f, UI_ARGB(208, 10, 13, 18));
    _snprintf(line, sizeof(line) - 1, "PERF %s %dx%d  MSAA %dx", Gfx_VideoModeStr(),
        Gfx_Width(), Gfx_Height(), Gfx_MsaaSamples());
    line[sizeof(line) - 1] = 0;
    Font_DrawText(d, 14.0f, 58.0f, line, FONT_SIZE_SMALL, txt, 284);
    _snprintf(line, sizeof(line) - 1, "FPS %lu.%lu  frame %lu.%lums peak %lums  >budget %lu",
        (unsigned long)(s / 10u), (unsigned long)(s % 10u),
        (unsigned long)(s_perfSnapshot.avgFrameMs10 / 10u),
        (unsigned long)(s_perfSnapshot.avgFrameMs10 % 10u),
        (unsigned long)s_perfSnapshot.peakFrameMs,
        (unsigned long)s_perfSnapshot.spikeCount);
    line[sizeof(line) - 1] = 0;
    Font_DrawText(d, 14.0f, 82.0f, line, FONT_SIZE_SMALL, txt, 284);
    _snprintf(line, sizeof(line) - 1, "FTP %lu.%lums / %lums peak",
        (unsigned long)(s_perfSnapshot.avgFtpMs10 / 10u),
        (unsigned long)(s_perfSnapshot.avgFtpMs10 % 10u),
        (unsigned long)s_perfSnapshot.peakFtpMs);
    line[sizeof(line) - 1] = 0;
    Font_DrawText(d, 14.0f, 104.0f, line, FONT_SIZE_SMALL, txt, 284);
    _snprintf(line, sizeof(line) - 1, "DRAW %lu.%lums / %lums peak  PRESENT %lums peak",
        (unsigned long)(s_perfSnapshot.avgRenderMs10 / 10u),
        (unsigned long)(s_perfSnapshot.avgRenderMs10 % 10u),
        (unsigned long)s_perfSnapshot.peakRenderMs,
        (unsigned long)s_perfSnapshot.peakPresentMs);
    line[sizeof(line) - 1] = 0;
    Font_DrawText(d, 14.0f, 126.0f, line, FONT_SIZE_SMALL, txt, 284);
}
#endif /* DD_PERF_HUD */

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
#define REVEAL_MS       180u    /* tiny target-screen settle after the concealed cut */
#define MODAL_OPEN_MS   150u    /* Recents / Power slide+fade in */
#define MODAL_CLOSE_MS   90u    /* close faster than open so input still feels instant */

                                   /* idle animation: after this long with no input, the orb occasionally glitches
                                      on its own. Each burst lasts IDLE_GLITCH_MS; the gap between bursts is
                                      randomised in [IDLE_GAP_MIN, IDLE_GAP_MIN+IDLE_GAP_RAND). */
#define IDLE_MS        8000
#define IDLE_GLITCH_MS 300
#define IDLE_GAP_MIN   4000
#define IDLE_GAP_RAND  7000

#include "dd_custom.h"
#include "dd_customadd.h"

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

/* ---- dynamic main menu rows: 4 builtin categories + custom + 3 system ---- */
#define MENU_VIS       7
#define MENU_MAX_ROWS  (4 + CUSTOM_MAX + 3)
enum { MROW_CAT_BUILTIN = 0, MROW_CAT_CUSTOM, MROW_FILEMAN, MROW_SAVEMGR, MROW_SETTINGS };
typedef struct { const char* label; int kind; int idx; } MenuRow;

static MenuRow s_mrows[MENU_MAX_ROWS];
static int     s_mrowCount = 0;
static int     s_menuScroll = 0;

static int BuildMenuRows(MenuRow* r) {
    int n = 0, i, cc;
    for (i = 0; i < 4; i++) { r[n].label = k_menu[i]; r[n].kind = MROW_CAT_BUILTIN; r[n].idx = i; n++; }
    cc = Custom_Count();
    for (i = 0; i < cc && n < MENU_MAX_ROWS - 3; i++) {
        const LauncherConfig* c = Custom_Get(i);
        r[n].label = c ? c->title : "?"; r[n].kind = MROW_CAT_CUSTOM; r[n].idx = i; n++;
    }
    r[n].label = k_menu[4]; r[n].kind = MROW_FILEMAN;  r[n].idx = 0; n++;
    r[n].label = k_menu[5]; r[n].kind = MROW_SAVEMGR;  r[n].idx = 0; n++;
    r[n].label = k_menu[6]; r[n].kind = MROW_SETTINGS; r[n].idx = 0; n++;
    return n;
}

static const LauncherConfig* RowConfig(const MenuRow* row) {
    if (row->kind == MROW_CAT_BUILTIN) return MenuConfig(row->idx);
    if (row->kind == MROW_CAT_CUSTOM)  return Custom_Get(row->idx);
    return 0;
}

static void MenuRebuild(void) { s_mrowCount = BuildMenuRows(s_mrows); }

/* ambient bloom now lives in dd_backdrop (shared across all screens) */

/* --- header status ticker: temp / fan / IP / free space ---------------- */
static char  s_status[288] = "DARKDASH";
static DWORD s_lastStatus = 0;
static int   s_ntpDone = 0;    /* one-shot internet time sync at boot */
static DWORD s_lastDiscPoll = 0; /* dedicated cadence for optical-disc detection */

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
        int di;
        for (di = 0; di < DD_Storage_Count(); di++) {
            const DD_StorageVolume* v = DD_Storage_At(di);
            char root[4], label[32], disk[16];
            if (!v || (v->partition != 1 && v->partition != 6 && v->partition != 7)) continue;
            if (!DD_Storage_Ready(v)) continue;
            root[0] = v->letter; root[1] = ':'; root[2] = '\\'; root[3] = 0;
            Sys_DiskFreeStr(root, disk, sizeof(disk));
            if (disk[0] == 0 || disk[0] == '-') continue;
            DD_Storage_Label(v, label, sizeof(label));
            StrAppend(s_status, sizeof(s_status), label);
            StrAppend(s_status, sizeof(s_status), " ");
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
   /* motion helpers for lightweight overlays / screen arrival. These deliberately
      use only a handful of 2D fills/sprites: visible polish with negligible NV2A
      cost compared with the normal dashboard render. */
static float EaseOut01(DWORD elapsed, DWORD duration) {
    float t, inv;
    if (duration == 0 || elapsed >= duration) return 1.0f;
    t = (float)elapsed / (float)duration;
    inv = 1.0f - t;
    return 1.0f - inv * inv;
}

static DWORD MulColorAlpha(DWORD c, int opacity) {
    int a = (int)((c >> 24) & 0xFF);
    if (opacity < 0) opacity = 0;
    if (opacity > 255) opacity = 255;
    a = a * opacity / 255;
    return (c & 0x00FFFFFFu) | ((DWORD)a << 24);
}

static float ModalProgress(DWORD openAt, int closing, DWORD closeAt) {
    DWORD now = GetTickCount();
    if (closing) {
        float p = EaseOut01(now - closeAt, MODAL_CLOSE_MS);
        return 1.0f - p;
    }
    return EaseOut01(now - openAt, MODAL_OPEN_MS);
}

static void DrawScreenReveal(DWORD revealAt) {
    DWORD now, e;
    float p, inv, x;
    int darkA, beamA;
    DWORD accent;
    int ar, ag, ab;
    if (!revealAt) return;
    now = GetTickCount();
    e = now - revealAt;
    if (e >= REVEAL_MS) return;
    p = (float)e / (float)REVEAL_MS;
    inv = 1.0f - p;
    darkA = (int)(72.0f * inv);
    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(darkA, 0, 0, 0));

    /* a quick accent scan makes the hidden door-cut read as an intentional
       reveal instead of a hard scene replacement. */
    accent = Theme_Color("accent", 0xFF7FE000);
    ar = (int)((accent >> 16) & 0xFF);
    ag = (int)((accent >> 8) & 0xFF);
    ab = (int)(accent & 0xFF);
    x = -64.0f + p * 768.0f;
    beamA = (int)(95.0f * inv);
    UI_FillRect(x, 0.0f, 6.0f, 480.0f, UI_ARGB(beamA, ar, ag, ab));
    UI_FillRect(x - 18.0f, 0.0f, 18.0f, 480.0f, UI_ARGB(beamA / 3, ar, ag, ab));
}

static void DrawRecents(int selRow, DWORD openAt, int closing, DWORD closeAt) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    /* Use an authored recent-activity panel, not the vertical menu art
       stretched onto a floating dialog. Fall back for older themes. */
    const Texture* frame = Theme_Asset("frame_recent");
    float bx = 200.0f, by = 120.0f, bw = 240.0f, bh = 240.0f;
    if (!frame) frame = Theme_RoleAsset("ui.menu.frame");
    int   rc = Recents_Count();
    int   i, opacity;
    float mp = ModalProgress(openAt, closing, closeAt);

    by += (1.0f - mp) * 10.0f;
    opacity = (int)(255.0f * mp);
    accent = MulColorAlpha(accent, opacity);
    text = MulColorAlpha(text, opacity);
    dim = MulColorAlpha(dim, opacity);
    glow = MulColorAlpha(glow, opacity);

    /* dim ramps with the panel; it never steals a full frame on open/close. */
    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB((int)(150.0f * mp), 0, 0, 0));
    UI_DrawPanelDepthMasked(frame, bx, by, bw, bh, opacity, 24.0f, 22.0f, 24.0f, 22.0f);
    if (frame) UI_DrawNineSlice(frame, bx, by, bw, bh,
        24.0f, 22.0f, 24.0f, 22.0f, UI_ARGB(opacity, 255, 255, 255), 0);
    else       UI_FillRect(bx, by, bw, bh, UI_ARGB((int)(235.0f * mp), 18, 22, 18));
    UI_DrawPanelLightingMasked(frame, bx, by, bw, bh, opacity, 24.0f, 22.0f, 24.0f, 22.0f);

    Font_DrawText(d, bx + 20.0f, by + 16.0f, "RECENT", FONT_SIZE_SMALL, accent, 0);

    if (rc == 0) {
        Font_DrawText(d, bx + 20.0f, by + 56.0f, "Nothing launched yet", FONT_SIZE_SMALL, dim, (int)(bw - 40.0f));
    }
    else {
        for (i = 0; i < rc; i++) {
            float ry = by + 48.0f + (float)i * 30.0f;
            if (i == selRow) {
                Select_BeginModal(0x6101, selRow);
                Select_DrawModalBoxFlat(bx + 12.0f, ry - 2.0f, bw - 24.0f, 26.0f,
                    UI_ARGB(100 * opacity / 255, ar, ag, ab));
            }
            Font_DrawText(d, bx + 22.0f, ry, Recents_Name(i), FONT_SIZE_SMALL,
                (i == selRow) ? glow : text, (int)(bw - 44.0f));
        }
    }
    DD_Hints_Draw(d, bx + 20.0f, by + bh - 33.0f,
        bw - 40.0f, 21.0f, "A LAUNCH  B CLOSE", dim);
}

/* Power menu overlay (WHITE tap on the main menu). Themed frame, same treatment
   as the recents overlay. Three options: Restart / Reboot / Shutdown. */
static void DrawPowerMenu(int selRow, DWORD openAt, int closing, DWORD closeAt) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD glow = Theme_Color("glow", 0xFFAEFF3C);
    int   ar = (int)((accent >> 16) & 0xFF), ag = (int)((accent >> 8) & 0xFF), ab = (int)(accent & 0xFF);
    const Texture* frame = Theme_Asset("frame_menu_h");
    static const char* const k_pow[3] = { "Restart", "Reboot", "Shutdown" };
    float bx = 232.0f, by = 150.0f, bw = 176.0f, bh = 158.0f;
    int   i, opacity;
    float mp = ModalProgress(openAt, closing, closeAt);
    if (!frame) frame = Theme_RoleAsset("ui.menu.frame");

    by += (1.0f - mp) * 10.0f;
    opacity = (int)(255.0f * mp);
    accent = MulColorAlpha(accent, opacity);
    text = MulColorAlpha(text, opacity);
    dim = MulColorAlpha(dim, opacity);
    glow = MulColorAlpha(glow, opacity);

    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB((int)(150.0f * mp), 0, 0, 0));
    UI_DrawPanelDepthMasked(frame, bx, by, bw, bh, opacity, 24.0f, 22.0f, 24.0f, 22.0f);
    if (frame) UI_DrawNineSlice(frame, bx, by, bw, bh,
        24.0f, 22.0f, 24.0f, 22.0f, UI_ARGB(opacity, 255, 255, 255), 0);
    else       UI_FillRect(bx, by, bw, bh, UI_ARGB((int)(235.0f * mp), 18, 22, 18));
    UI_DrawPanelLightingMasked(frame, bx, by, bw, bh, opacity, 24.0f, 22.0f, 24.0f, 22.0f);

    Font_DrawText(d, bx + 20.0f, by + 16.0f, "POWER", FONT_SIZE_SMALL, accent, 0);
    for (i = 0; i < 3; i++) {
        float ry = by + 46.0f + (float)i * 28.0f;
        if (i == selRow) {
            Select_BeginModal(0x6102, selRow);
            Select_DrawModalBoxFlat(bx + 12.0f, ry - 2.0f, bw - 24.0f, 24.0f,
                UI_ARGB(100 * opacity / 255, ar, ag, ab));
        }
        Font_DrawText(d, bx + 22.0f, ry, k_pow[i], FONT_SIZE_SMALL,
            (i == selRow) ? glow : text, 0);
    }
    DD_Hints_Draw(d, bx + 20.0f, by + bh - 27.0f,
        bw - 40.0f, 20.0f, "A OK  B CLOSE", dim);
}

static void DrawSplash(int sel, int glowAlpha, int glitch) {
    IDirect3DDevice8* d = Gfx_Device();
    DWORD accent = Theme_Color("accent", 0xFF7FE000);
    DWORD dim = Theme_Color("text_dim", 0xFF7FA060);
    DWORD text = Theme_Color("text", 0xFFD8F8C0);
    DWORD glow = Theme_GlowColor();      /* [glow] color, falls back to palette.glow */
    const Texture* orb = Theme_RoleAsset("stage.orb.body");
    const Texture* orbGlow = Theme_RoleAsset("stage.orb.glow");
    const ThemeStageLighting* stageLight = Theme_StageLighting();
    const Texture* hdr = Theme_RoleAsset("ui.header.frame");
    const Texture* menu = Theme_RoleAsset("ui.menu.frame");
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
    {
        /* The environment eases toward the selected visible row. This moves
           ONLY background/light layers; icons, text and plasma stay stable. */
        float focus = (float)(sel - s_menuScroll) / (float)(MENU_VIS - 1);
#if DD_A4_DEPTH
        Backdrop_DrawHome(focus);
#else
        (void)focus;
        Backdrop_Draw();
#endif
    }

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
    else if (Data_FxOn(DD_FX_PLASMA)) {
        /* Plasma orb: procedural energy ball replaces the static hero orb,
           tinted from the theme. Occupies the same flat orb rect. (Skips the
           transition glitch -- the plasma is always in motion anyway.) */
        TRACED("ds.plasma", ">");
        Plasma_Draw(44.0f, 168.0f, 272.0f, 234.0f, accent, glow);
        TRACED("ds.plasma", "<");
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
            /* Optional separate emissive mask. Without it, retain the old
               full-orb glow for legacy theme compatibility. */
            if (orbGlow) {
                int oa = glowAlpha * stageLight->emissiveLevel / 100;
                if (oa > 180) oa = 180;
                if (oa > 0) UI_DrawSprite(orbGlow, 44.0f, 168.0f, 272.0f, 234.0f,
                    ((DWORD)oa << 24) | (stageLight->emissive & 0x00FFFFFF), 1);
            }
            else {
                UI_DrawSprite(orb, 44.0f, 168.0f, 272.0f, 234.0f,
                    UI_ARGB(glowAlpha, 255, 255, 255), 1);
            }
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

    /* --- "Now Playing" toast under the pedestal: appears on a Shuffle track
       change and fades itself out. Two centred lines in the hero column. --- */
    {
        const char* np = 0;
        float npA = 0.0f;
        if (Settings_NowPlaying(&np, &npA) && np && np[0]) {
            float cx = 180.0f, maxW = 264.0f;
            float lh = (float)Font_LineHeight(FONT_SIZE_SMALL);
            float ly = 396.0f;
            int   a = (int)(npA * 255.0f);
            int   lw = Font_MeasureText("NOW PLAYING", FONT_SIZE_SMALL);
            int   nw = Font_MeasureText(np, FONT_SIZE_SMALL);
            DWORD cl, cn;
            if (a > 255) a = 255;
            if (a < 0) a = 0;
            cl = ((DWORD)a << 24) | (accent & 0x00FFFFFF);
            cn = ((DWORD)a << 24) | (text & 0x00FFFFFF);
            if ((float)nw > maxW) nw = (int)maxW;
            Font_DrawText(d, cx - (float)lw * 0.5f, ly, "NOW PLAYING", FONT_SIZE_SMALL, cl, 0);
            Font_DrawText(d, cx - (float)nw * 0.5f, ly + lh, np, FONT_SIZE_SMALL, cn, (int)maxW);
        }
    }

    /* --- flat top-left header bar: frame + live status ticker --- */
    if (hdr) UI_DrawNineSlice(hdr, 8.0f, 8.0f, 300.0f, 40.0f,
        22.0f, 10.0f, 22.0f, 10.0f, 0xFFFFFFFF, 0);
    DrawStatusScroller(d);

    /* --- disc popup (top-right): shown only when a game disc is present --- */
    {
        const DiscState* ds = Disc_Get();
        if (ds->present && ds->isXboxGame) {
            const Texture* dbar = Theme_RoleAsset("ui.header.frame");
            float bx = 380.0f, by = 8.0f, bw = 252.0f, bh = 40.0f;
            float gh = (float)Font_GlyphHeight(FONT_SIZE_SMALL);
            float ty = by + (bh - gh) * 0.5f;        /* vertically centre in the bar */
            if (ty < by + 2.0f) ty = by + 2.0f;
            if (dbar) UI_DrawNineSlice(dbar, bx, by, bw, bh,
                22.0f, 10.0f, 22.0f, 10.0f, 0xFFFFFFFF, 0);
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
        /* Frame arcs: occasional Jacob's-ladder bolts hugging the border, drawn
           BEFORE the panel so the frame occludes their roots. Only at rest (not
           mid-swing, where the capture would clip them) and only when enabled. */
        if (!turning && Data_FxOn(DD_FX_ARCS)) {
            Arcs_Tick(GetTickCount());
            TRACED("ds.arcs", ">");
            Arcs_Draw(menuX, menuY, 272.0f, 384.0f);
            TRACED("ds.arcs", "<");
        }
        if (menu) Iso_DrawPanel(menu, menuX, menuY, 272.0f, 384.0f, 0xFFFFFFFF, 0);
        /* Common translucent selection box: the authored selection PNG can
           contain hard opaque borders that obscure menu text when shrunk and
           additively blended.  The same alpha-blended geometry is now used by
           the launcher, settings, file manager and save manager.  Keep the
           highlight off the swing capture and let Swing_RectAlpha fade it. */
        if (!turning && rectA > 0 &&
            sel >= s_menuScroll && sel < s_menuScroll + MENU_VIS) {
            float gy = rowY0 + rowDY * (float)(sel - s_menuScroll) - 6.0f;
            int ha = 100 * rectA / 255;
            Select_Begin(0x1000, gy);
            if (ha > 0) {
                TRACED("ds.glow", ">");
                Select_DrawGlow(menuX + 18.0f, gy, 210.0f, 36.0f,
                    UI_ARGB(ha, ar, ag, ab));
                TRACED("ds.glow", "<");
            }
        }
        for (i = 0; i < MENU_VIS && (s_menuScroll + i) < s_mrowCount; i++) {
            int ri = s_menuScroll + i;
            DWORD c = (ri == sel) ? glow : text;
            Font_DrawTextIso(d, menuX + 30.0f, rowY0 + rowDY * (float)i + 4.0f,
                s_mrows[ri].label, FONT_SIZE_MEDIUM, c);
        }
        Iso_End();

        if (captured) {
            /* frame+text captured -> restore backbuffer -> swing the quad */
            Swing_CaptureEnd();
            Swing_DrawCaptured(menuX, menuY, 272.0f, 384.0f);
        }
    }

    /* --- footer status bar: FLAT --- */
    {
        int customSel = (sel >= 0 && sel < s_mrowCount && s_mrows[sel].kind == MROW_CAT_CUSTOM);
        const char* hint = customSel
            ? "A SELECT  L3 JUKEBOX  R3 REMOVE  Y RECENTS  X ADD CATEGORY"
            : "A SELECT  L3 JUKEBOX  Y RECENTS  WHITE POWER  START DISC  X ADD";
        if (DD_Hints_Measure(hint) > 590.0f)
            hint = customSel
            ? "A SELECT  L3 JUKEBOX  R3 REMOVE  Y RECENTS"
            : "A SELECT  L3 JUKEBOX  Y RECENTS  WHITE POWER";
        DD_Hints_Footer(d, 442.0f, 32.0f, 24.0f, 590.0f, hint, text);
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
    DWORD recOpenAt = 0, recCloseAt = 0;
    int   recClosing = 0;
    int   powOpen = 0;          /* power menu overlay (WHITE tap) */
    int   powSel = 0;
    DWORD powOpenAt = 0, powCloseAt = 0;
    int   powClosing = 0;
    DWORD revealAt = 0;         /* target-screen reveal after the hidden cut */
    int   saverOn = 0;          /* screensaver active */
    DWORD whiteDownMs = 0;      /* when WHITE went down (0 = not down) */
    int   whiteMoved = 0;      /* DPAD touched during this WHITE hold? */

    /* Point D: at our own install folder BEFORE anything reads "D:\...". When
       booted as the dashboard (Cerbios) the kernel's D: may be absent/wrong, so
       without this every asset/theme/data path fails to resolve. */
    Mount_SelfToD();
    Trace_Init();            /* brute-force disk tracer: flush every line to D:\data\ddtrace.log */
    Data_Load();             /* load prefs first: Gfx_Init reads videoRes from it */
    Recents_Init();          /* most-recently-launched titles (Y overlay) */
    Paths_Load();            /* user-added custom scan paths per category */
    Custom_Load();           /* user-defined launcher categories (custom.dat) */
    MenuRebuild();           /* assemble the menu rows (builtins + custom)    */
    Time_Load();             /* NTP enable + timezone prefs (time.dat)        */
    if (!Gfx_Init()) return;
    UI_Init(Gfx_Width(), Gfx_Height());
    Calib_Apply();           /* apply saved overscan insets (zero if uncalibrated) */
    Font_Init(Gfx_Device());
    InitInput();
    Mount_HddPartitions();   /* unchanged HDD0 mappings; X/Y/Z cache included */
    DD_Storage_ProbeSecondary(); /* CerBIOS HDD1 object discovery */
    DD_Storage_MountSecondary(); /* only accessible roots; no alias overwrite */

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
    Jukebox_Init();           /* persistent root/playlists; audio stays owned by BGM until L3 */
    Settings_StartMusic(1);   /* honor the saved choice: built-in / custom / None / Shuffle */

    /* FTP service: mark it wanted if enabled -- it will bring itself up once
       the network/DHCP is actually ready (Net_IsUp is false this early at boot,
       so we can't start it here; Ftp_Tick handles the deferred start). */
    Ftp_Init();
    if (Data_Get()->ftpEnabled)
        Ftp_Want(1);

    Lcd_Init();              /* physical LCD accessory: load lcd.dat, probe, splash */
    Udp_Init();              /* shared UDP layer for DarkoneCustoms accessories */
    TypeD_Init();            /* Type-D status broadcaster (UDP 50504)           */
    Watchdog_Init();         /* arm main-thread liveness watchdog (SMBus wedge recovery) */
    Smbsvc_Init();           /* start the SMBus service thread: it now owns ALL bus I/O
                                (sensor reads + LCD writes), keeping the render loop bus-free */
    Oxfp_Init();             /* OXFP front-panel control (UDP 32123)            */
    Rgb_Init();              /* XBOX-RGB control (UDP 7777)                     */
    XView_RefreshTheme();    /* snapshot theme colors for the X-View panel      */
    XView_Start();           /* start the X-View panel thread (if enabled)      */

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
#if DD_PERF_HUD
        DWORD perfBegin, perfDrawBegin, perfPresentBegin;
        Perf_BeginLoop();
#endif

        PumpInput();
        btn = GetButtons();

        /* optical-disc detection runs on EVERY screen, every ~500ms, decoupled
           from the 1/sec header-status refresh below. A game disc inserted while
           in any submenu (launcher, file manager, settings, save manager), while
           an overlay is open, or mid-transition is picked up and mounted just the
           same. dd_disc internally throttles the mount/probe, so this only costs
           one cheap SMC tray read per tick. */
        {
            DWORD dpt = GetTickCount();
            if (s_lastDiscPoll == 0 || dpt - s_lastDiscPoll >= 500) {
                s_lastDiscPoll = dpt;
                Disc_Poll();          /* mount/unmount a disc on tray change */
            }
        }

#if DD_PERF_HUD
        perfBegin = GetTickCount();
#endif
        Ftp_Tick();   /* service the FTP server every frame (single-threaded) */
#if DD_PERF_HUD
        Perf_RecordCost(GetTickCount() - perfBegin, &s_perfFtpTotal, &s_perfFtpPeak);
#endif
        TypeD_Tick(); /* broadcast Type-D status (background, rate-limited)       */
        Udp_DiscoTick(); /* discover XBOX-RGB / OXFP for accessory menu gating    */
        TypeDArt_BootResumeTick(); /* once discovered, drop last session's held art */
        /* Boot-time NTP is a true frame-pumped state machine: DNS and UDP are
           polled without Sleep/select timeouts, so a dead time server cannot
           stall input, FTP, rendering, or accessory service. */
        if (!s_ntpDone && Ntp_AsyncBusy()) {
            if (!Time_NtpEnabled()) {
                Ntp_AsyncCancel();            /* respect a runtime preference change */
            }
            else {
                int nr = Ntp_AsyncPoll();
                if (nr != 0) s_ntpDone = 1;   /* success or all servers exhausted */
            }
        }
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
                screen == SCR_MAIN && trans == 0 && !Jukebox_IsOpen() &&
                !recOpen && !powOpen && !CustomAdd_IsOpen()) {
                powSel = 0; powOpen = 1; powClosing = 0; powOpenAt = GetTickCount(); Audio_PlaySfx(SFX_SELECT);
            }
            whiteDownMs = 0;
        }
        if (trans != 0) {
            /* transition animating -- swallow menu input until it settles */
        }
        else if (screen == SCR_MAIN && Jukebox_IsOpen()) {
            /* Jukebox owns all main-screen input while open. This must precede
               WHITE tuning/power handling because WHITE is Next Track here. */
            if (Jukebox_Update(pressed, btn)) {
                lastInputMs = GetTickCount();
                Fx_FlashEdge();
            }
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
            if (CustomAdd_IsOpen()) {
                int car = CustomAdd_Update(pressed);
                if (car == 1) MenuRebuild();   /* category added -> show it now */
            }
            else if (powOpen) {
                /* power menu owns input while open; closing frames swallow input. */
                if (!powClosing && (pressed & BTN_DPAD_DOWN)) { if (powSel < 2) { powSel++; Audio_PlaySfx(SFX_NAV_DOWN); } }
                if (!powClosing && (pressed & BTN_DPAD_UP)) { if (powSel > 0) { powSel--; Audio_PlaySfx(SFX_NAV_UP); } }
                if (!powClosing && (pressed & BTN_A)) {
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
                if (!powClosing && (pressed & BTN_B)) { powClosing = 1; powCloseAt = GetTickCount(); Audio_PlaySfx(SFX_BACK); }
            }
            else if (recOpen) {
                /* recents overlay owns input while open; closing frames swallow input. */
                int rc = Recents_Count();
                if (!recClosing && (pressed & BTN_DPAD_DOWN)) { if (recSel < rc - 1) { recSel++; Audio_PlaySfx(SFX_NAV_DOWN); } }
                if (!recClosing && (pressed & BTN_DPAD_UP)) { if (recSel > 0) { recSel--; Audio_PlaySfx(SFX_NAV_UP); } }
                if (!recClosing && (pressed & BTN_A) && rc > 0) {
                    const char* path = Recents_Path(recSel);
                    Audio_PlaySfx(SFX_SELECT);
                    if (path && path[0]) {
                        Audio_StopMusic();
                        Mount_LaunchXbe(path);   /* no return on success */
                        /* fell through -> launch failed; close and carry on */
                    }
                    recOpen = 0;
                }
                if (!recClosing && (pressed & (BTN_B | BTN_Y))) { recClosing = 1; recCloseAt = GetTickCount(); Audio_PlaySfx(SFX_BACK); }
            }
            else {
                /* L3 opens the lightweight Jukebox in-place over the main scene.
                   Swallow this frame's other presses so L3+A cannot also launch
                   the selected menu row. */
                if (pressed & BTN_LTHUMB) {
                    Jukebox_Enter();
                    Audio_PlaySfx(SFX_ALT);
                    pressed = 0;
                    lastInputMs = GetTickCount();
                }

                /* R3 on a custom row removes only the persisted menu/category
                   entry. The category's source folder and files are untouched. */
                if ((pressed & BTN_RTHUMB) && sel >= 0 && sel < s_mrowCount &&
                    s_mrows[sel].kind == MROW_CAT_CUSTOM) {
                    if (Custom_Remove(s_mrows[sel].idx)) {
                        MenuRebuild();
                        if (sel >= s_mrowCount) sel = s_mrowCount - 1;
                        if (sel < 0) sel = 0;
                        if (s_menuScroll > sel) s_menuScroll = sel;
                        if (s_menuScroll > s_mrowCount - MENU_VIS)
                            s_menuScroll = s_mrowCount - MENU_VIS;
                        if (s_menuScroll < 0) s_menuScroll = 0;
                        Audio_PlaySfx(SFX_BACK);
                    }
                    pressed = 0;
                }

                if (pressed & BTN_DPAD_DOWN) { if (sel < s_mrowCount - 1) { sel++; Audio_PlaySfx(SFX_NAV_DOWN); } }
                if (pressed & BTN_DPAD_UP) { if (sel > 0) { sel--; Audio_PlaySfx(SFX_NAV_UP); } }
                if (sel < s_menuScroll) s_menuScroll = sel;
                if (sel >= s_menuScroll + MENU_VIS) s_menuScroll = sel - MENU_VIS + 1;
                if (pressed & BTN_A) {
                    const MenuRow* row = &s_mrows[sel];
                    const LauncherConfig* cfg = RowConfig(row);
                    Audio_PlaySfx(SFX_SELECT);
                    Fx_FlashEdge();                      /* edge-glow pulse on select */
                    if (cfg) {                           /* a browser row: glitch out, then open */
                        pendingCfg = cfg;
                        trans = 1; transStart = GetTickCount(); transTarget = SCR_LAUNCH;
                        Swing_Start();                       /* door-swing the menu out */
                    }
                    else if (row->kind == MROW_FILEMAN) {       /* FILE MANAGER */
                        trans = 1; transStart = GetTickCount(); transTarget = SCR_FILEMAN;
                        Swing_Start();
                    }
                    else if (row->kind == MROW_SAVEMGR) {       /* SAVE MANAGER */
                        trans = 1; transStart = GetTickCount(); transTarget = SCR_SAVEMGR;
                        Swing_Start();
                    }
                    else if (row->kind == MROW_SETTINGS) {      /* SETTINGS */
                        trans = 1; transStart = GetTickCount(); transTarget = SCR_SETTINGS;
                        Swing_Start();
                    }
                    /* (no rows past SETTINGS) */
                }
                if (pressed & BTN_X) {            /* X = add a custom category */
                    Audio_PlaySfx(SFX_SELECT);
                    CustomAdd_Open();
                }
                if (pressed & BTN_B) Audio_PlaySfx(SFX_BACK);
                if (pressed & BTN_START) {           /* launch a present game disc */
                    const DiscState* ds = Disc_Get();
                    if (ds->present && ds->isXboxGame) {
                        Audio_PlaySfx(SFX_SELECT);
                        Disc_Launch();               /* no return on success */
                    }
                }
                if (pressed & BTN_Y) {               /* open the recents overlay */
                    Recents_PruneMissing();           /* don't offer moved/deleted titles */
                    recSel = 0;
                    recOpen = 1; recClosing = 0; recOpenAt = GetTickCount();
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

        /* let modal close motion finish before dropping the overlay state. */
        if (powOpen && powClosing && GetTickCount() - powCloseAt >= MODAL_CLOSE_MS) {
            powOpen = 0; powClosing = 0;
        }
        if (recOpen && recClosing && GetTickCount() - recCloseAt >= MODAL_CLOSE_MS) {
            recOpen = 0; recClosing = 0;
        }

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

            /* one-shot internet time sync: once DHCP is ready, START the async
               attempt. Ntp_AsyncPoll above advances it every frame. */
            if (!s_ntpDone && Time_NtpEnabled()) {
                if (Net_IsUp()) {
                    if (!Ntp_AsyncBusy() && !Ntp_AsyncBegin()) s_ntpDone = 1;
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
                revealAt = t;                          /* soften the concealed cut */
                Fx_FlashEdge();                         /* arrival pulse, if edge FX enabled */
                trans = 0; glitch = 0;
            }
        }
        else if (trans == 2) {          /* glitch IN, settling on main */
            el = t - transStart;
            glitch = 100 - (int)(el * 100 / TRANS_MS);
            if (el >= TRANS_MS) { trans = 0; glitch = 0; Fx_FlashEdge(); }
        }
        if (glitch < 0)   glitch = 0;
        if (glitch > 100) glitch = 100;

        /* idle animation: on the main menu, with no transition and no input
           for a while, fire an occasional self-glitch. It only writes 'glitch'
           (no screen change), so navigation stays live throughout. */
        if (screen == SCR_MAIN && trans == 0 && !Jukebox_IsOpen()) {
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
            if (ssMin > 0 && screen == SCR_MAIN && trans == 0 && !Jukebox_IsOpen() &&
                !recOpen && !powOpen && !CustomAdd_IsOpen() &&
                (t - lastInputMs) > (DWORD)ssMin * 60000u) {
                Saver_Enter();
                saverOn = 1;
            }
        }
        if (saverOn) Saver_Update();

        /* publish the screensaver countdown to X-View so it can pre-dim its
           backlight before the saver's art decode starves the panel thread.
           0 = active now, <0 = cannot fire from the current screen (stay lit). */
        {
            int ssCd = -1, ssMin2 = Data_Get()->screensaverMin;
            if (saverOn) ssCd = 0;
            else if (ssMin2 > 0 && screen == SCR_MAIN && trans == 0 && !Jukebox_IsOpen() &&
                !recOpen && !powOpen && !CustomAdd_IsOpen()) {
                DWORD thr = (DWORD)ssMin2 * 60000u, idle = t - lastInputMs;
                ssCd = (idle >= thr) ? 0 : (int)(thr - idle);
            }
            XView_SetSaverCountdown(ssCd);
        }

        TRACE("frame", "audio>");
        Audio_Update();   /* service Xbox DS mixer every frame */
        TRACE("frame", "music>");
        if (!Jukebox_IsOpen())
            Settings_MusicTick();   /* BGM Shuffle yields while Jukebox owns the stream */
        TRACED("frame", "render>");
#if DD_PERF_HUD
        perfDrawBegin = GetTickCount();
#endif
        Gfx_BeginFrame(Theme_BG());
        TRACEDU("r.begin<", saverOn ? "saver" : "screen", (unsigned long)screen);
        if (saverOn) {
            Saver_Render();
        }
        else {
            if (screen == SCR_LAUNCH)       Launcher_Render();
            else if (screen == SCR_FILEMAN) FileMan_Render();
            else if (screen == SCR_SETTINGS) Settings_Render();
            else if (screen == SCR_SAVEMGR) SaveMgr_Render();
            else                            DrawSplash(sel, glowA, glitch);
            if (screen == SCR_MAIN && Jukebox_IsOpen()) Jukebox_Render();
            if (screen == SCR_MAIN && recOpen) DrawRecents(recSel, recOpenAt, recClosing, recCloseAt);
            if (screen == SCR_MAIN && powOpen) DrawPowerMenu(powSel, powOpenAt, powClosing, powCloseAt);
            if (screen == SCR_MAIN && CustomAdd_IsOpen()) CustomAdd_Draw(Gfx_Device());
        }
        TRACED("r.draw<", "");
        if (!saverOn && screen != SCR_MAIN) DrawScreenReveal(revealAt);
        /* ambient overlays, over all content: CRT scanlines + roll, then the
           SFX-synced edge flash. The boot intro (if active) tops everything.
           Each gated by its DD_FX_* toggle. */
        if (Data_FxOn(DD_FX_SCANLINES)) { TRACE("r.fx", "scan>");  Fx_DrawScanlines(); }
        if (Data_FxOn(DD_FX_EDGE)) { TRACE("r.fx", "edge>");  Fx_DrawEdgeGlow(); }
        if (Fx_BootActive()) { TRACE("r.fx", "boot>");  Fx_DrawBoot(); }
#if DD_PERF_HUD
        /* Last in the scene: overlay never changes normal dashboard layout. */
        Perf_DrawHud();
        Perf_RecordCost(GetTickCount() - perfDrawBegin, &s_perfDrawTotal, &s_perfDrawPeak);
        perfPresentBegin = GetTickCount();
#endif
        TRACED("r.present", ">");
        Gfx_EndFrame();
#if DD_PERF_HUD
        Perf_RecordCost(GetTickCount() - perfPresentBegin,
            &s_perfPresentTotal, &s_perfPresentPeak);
#endif
        TRACED("frame", "end");
    }

    Smbsvc_Stop();           /* stop the SMBus service thread before tearing down */
    Ntp_AsyncCancel();       /* release a boot-time DNS/socket attempt, if any     */
    XView_Stop();            /* outro + shut down the X-View panel thread          */
    Ftp_Stop();
    Fx_Shutdown();
    Egg_Shutdown();
    Jukebox_Shutdown();
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