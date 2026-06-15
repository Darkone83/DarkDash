/*---------------------------------------------------------------------------
    dd_isoui.cpp -- see dd_isoui.h.

    A small state machine drawn as a themed overlay:
      ST_FOLDER : dd_browse picks the source folder (Browse draws itself)
      ST_PICK   : list the .iso files found there; pick one
      ST_ROOT   : pick the install drive (only if >1 games root is mounted)
      ST_BUSY   : one frame of "INSTALLING" before the (blocking) install
      ST_RESULT : outcome message

    C89 style, file-scope statics, no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_isoui.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "font.h"
#include "input.h"
#include "dd_audio.h"
#include "dd_browse.h"
#include "dd_isoinstall.h"
#include "dd_paths.h"
#include "games.h"

#define ISOUI_PATH_MAX   260
#define ISOUI_NAME_MAX   96
#define ISOUI_MAX_ISOS   64
#define ISOUI_MAX_ROOTS  8
#define ISOUI_VIS_ROWS   8

enum { ST_FOLDER = 0, ST_PICK, ST_ROOT, ST_BUSY, ST_RESULT };

static int  s_open = 0;
static int  s_state = ST_FOLDER;

static char s_folder[ISOUI_PATH_MAX];
static char s_isoNames[ISOUI_MAX_ISOS][ISOUI_NAME_MAX];
static int  s_isoCount = 0;
static int  s_isoSel = 0;

static char s_roots[ISOUI_MAX_ROOTS][ISOUI_PATH_MAX];
static int  s_rootCount = 0;
static int  s_rootSel = 0;

static char s_isoPath[ISOUI_PATH_MAX];
static char s_destRoot[ISOUI_PATH_MAX];

static int  s_busyArmed = 0;
static int  s_resultCode = 0;
static int  s_resultOk = 0;

#define RC_NOISO   (-100)   /* local: folder held no .iso */

/* ---- helpers ---------------------------------------------------------- */

static void StrCopy(char* d, int cap, const char* s) {
    int i = 0;
    if (cap <= 0) return;
    if (s) { while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } }
    d[i] = 0;
}

static void Join(char* out, int cap, const char* a, const char* b) {
    int i = 0, j = 0;
    if (cap <= 0) return;
    while (a[j] && i < cap - 1) out[i++] = a[j++];
    if (i > 0 && out[i - 1] != '\\' && i < cap - 1) out[i++] = '\\';
    j = 0;
    while (b[j] && i < cap - 1) out[i++] = b[j++];
    out[i] = 0;
}

/* already in the list? (case-insensitive) */
static int NameListed(const char* nm) {
    int i, j;
    for (i = 0; i < s_isoCount; i++) {
        const char* a = s_isoNames[i];
        j = 0;
        for (;;) {
            char ca = a[j], cb = nm[j];
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            if (ca == 0) return 1;   /* full match */
            j++;
        }
    }
    return 0;
}

/* case-insensitive "does s end with suf?" */
static int EndsWithI(const char* s, const char* suf) {
    int sl = 0, fl = 0, i;
    while (s[sl]) sl++;
    while (suf[fl]) fl++;
    if (sl < fl) return 0;
    for (i = 0; i < fl; i++) {
        char a = s[sl - fl + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* List the installable disc images in a folder. We enumerate "*" and filter by
   extension in code: the FATX FindFirstFile wildcard does NOT reliably match
   multi-dot names (e.g. "*.iso" misses "game.xiso.iso"), but "*" always works
   -- it's exactly what the launcher's directory scanner uses. */
static void ScanIsos(const char* folder) {
    char pattern[ISOUI_PATH_MAX];
    WIN32_FIND_DATA fd;
    HANDLE h;

    s_isoCount = 0;
    Join(pattern, sizeof(pattern), folder, "*");
    h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!EndsWithI(fd.cFileName, ".iso") && !EndsWithI(fd.cFileName, ".xiso")) continue;
        if (s_isoCount >= ISOUI_MAX_ISOS) break;
        if (NameListed(fd.cFileName)) continue;
        StrCopy(s_isoNames[s_isoCount], ISOUI_NAME_MAX, fd.cFileName);
        s_isoCount++;
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

/* collect the mounted games roots (custom "games" paths first, then E/F/G) */
static void GatherRoots(void) {
    const LauncherConfig* cfg;
    int np, k, i;

    s_rootCount = 0;
    np = Paths_Count("games");
    for (k = 0; k < np && s_rootCount < ISOUI_MAX_ROOTS; k++) {
        const char* p = Paths_Get("games", k);
        if (p && p[0] && IsoInstall_RootAccessible(p)) {
            StrCopy(s_roots[s_rootCount], ISOUI_PATH_MAX, p);
            s_rootCount++;
        }
    }
    cfg = Games_Config();
    if (cfg && cfg->roots) {
        for (i = 0; i < cfg->rootCount && s_rootCount < ISOUI_MAX_ROOTS; i++) {
            if (IsoInstall_RootAccessible(cfg->roots[i])) {
                StrCopy(s_roots[s_rootCount], ISOUI_PATH_MAX, cfg->roots[i]);
                s_rootCount++;
            }
        }
    }
}

static const char* ResultText(void) {
    switch (s_resultCode) {
    case ISOINST_OK:          return "Installed";
    case ISOINST_ERR_NOTXISO: return "Not a valid Xbox ISO";
    case ISOINST_ERR_NODEST:  return "No games drive available";
    case ISOINST_ERR_MKDIR:   return "Could not create folder";
    case ISOINST_ERR_NOXBE:   return "No default.xbe inside ISO";
    case ISOINST_ERR_ATTACH:  return "attach.xbe missing / build failed";
    case ISOINST_ERR_MOVE:    return "Could not move the ISO";
    case RC_NOISO:            return "No ISO files in that folder";
    default:                  return "Install failed";
    }
}

/* ---- lifecycle -------------------------------------------------------- */

void IsoUi_Open(void) {
    s_open = 1;
    s_state = ST_FOLDER;
    s_isoCount = 0; s_isoSel = 0;
    s_rootCount = 0; s_rootSel = 0;
    s_busyArmed = 0;
    s_folder[0] = 0; s_isoPath[0] = 0; s_destRoot[0] = 0;
    Browse_Open("Select the folder with the ISO");
}

void IsoUi_Close(void) {
    if (Browse_IsOpen()) Browse_Close();
    s_open = 0;
    s_state = ST_FOLDER;
}

int IsoUi_IsOpen(void) { return s_open; }

/* ---- update ----------------------------------------------------------- */

int IsoUi_Update(WORD pressed) {
    if (!s_open) return -1;

    if (s_state == ST_FOLDER) {
        int r = Browse_Update(pressed);
        if (r == 1) {
            Browse_GetPath(s_folder, sizeof(s_folder));
            ScanIsos(s_folder);
            if (s_isoCount == 0) { s_resultOk = 0; s_resultCode = RC_NOISO; s_state = ST_RESULT; }
            else { s_isoSel = 0; s_state = ST_PICK; }
        }
        else if (r == -1) { IsoUi_Close(); return -1; }
        return 0;
    }

    if (s_state == ST_PICK) {
        if (pressed & BTN_B) { Audio_PlaySfx(SFX_BACK); IsoUi_Close(); return -1; }
        if ((pressed & BTN_DPAD_DOWN) && s_isoSel < s_isoCount - 1) { s_isoSel++; Audio_PlaySfx(SFX_NAV_DOWN); }
        if ((pressed & BTN_DPAD_UP) && s_isoSel > 0) { s_isoSel--; Audio_PlaySfx(SFX_NAV_UP); }
        if (pressed & BTN_A) {
            Join(s_isoPath, sizeof(s_isoPath), s_folder, s_isoNames[s_isoSel]);
            GatherRoots();
            Audio_PlaySfx(SFX_SELECT);
            if (s_rootCount == 0) { s_resultOk = 0; s_resultCode = ISOINST_ERR_NODEST; s_state = ST_RESULT; }
            else if (s_rootCount == 1) { StrCopy(s_destRoot, sizeof(s_destRoot), s_roots[0]); s_busyArmed = 0; s_state = ST_BUSY; }
            else { s_rootSel = 0; s_state = ST_ROOT; }
        }
        return 0;
    }

    if (s_state == ST_ROOT) {
        if (pressed & BTN_B) { Audio_PlaySfx(SFX_BACK); s_state = ST_PICK; return 0; }
        if ((pressed & BTN_DPAD_DOWN) && s_rootSel < s_rootCount - 1) { s_rootSel++; Audio_PlaySfx(SFX_NAV_DOWN); }
        if ((pressed & BTN_DPAD_UP) && s_rootSel > 0) { s_rootSel--; Audio_PlaySfx(SFX_NAV_UP); }
        if (pressed & BTN_A) {
            StrCopy(s_destRoot, sizeof(s_destRoot), s_roots[s_rootSel]);
            Audio_PlaySfx(SFX_SELECT);
            s_busyArmed = 0; s_state = ST_BUSY;
        }
        return 0;
    }

    if (s_state == ST_BUSY) {
        if (!s_busyArmed) { s_busyArmed = 1; return 0; }   /* let one BUSY frame render first */
        s_resultCode = IsoInstall_Run(s_isoPath, s_destRoot);
        s_resultOk = (s_resultCode == ISOINST_OK);
        s_state = ST_RESULT;
        return 0;
    }

    if (s_state == ST_RESULT) {
        if (pressed & (BTN_A | BTN_B)) {
            int ok = s_resultOk;
            Audio_PlaySfx(ok ? SFX_SELECT : SFX_BACK);
            IsoUi_Close();
            return ok ? 1 : -1;
        }
        return 0;
    }

    return 0;
}

/* ---- draw ------------------------------------------------------------- */

static void Panel(IDirect3DDevice8* d, float* inL, float* inR, float* inT, float* inB) {
    const Texture* frame;
    float fx = 80.0f, fy = 80.0f, fw = 480.0f, fh = 320.0f;
    DWORD bg = Theme_Color("bg", 0xFF060A08);
    int br = (int)((bg >> 16) & 0xFF), bgg = (int)((bg >> 8) & 0xFF), bb = (int)(bg & 0xFF);

    UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(180, br / 3, bgg / 3, bb / 3));
    frame = Theme_Asset("frame_menu_v");
    if (frame) UI_DrawSprite(frame, fx, fy, fw, fh, 0xFFFFFFFF, 0);
    else       UI_FillRect(fx, fy, fw, fh, UI_ARGB(235, br, bgg, bb));

    *inL = fx + 32.0f; *inR = fx + fw - 32.0f;
    *inT = fy + 34.0f; *inB = fy + fh - 26.0f;
    (void)d;
}

static void DrawList(IDirect3DDevice8* d, float inL, float inT,
    const char* names, int count, int sel,
    DWORD text, DWORD glow, DWORD dim, int nameStride) {
    int first = 0, i, shown;
    float rowDY = 22.0f;
    (void)dim;
    if (sel >= ISOUI_VIS_ROWS) first = sel - (ISOUI_VIS_ROWS - 1);
    shown = count - first;
    if (shown > ISOUI_VIS_ROWS) shown = ISOUI_VIS_ROWS;
    for (i = 0; i < shown; i++) {
        int idx = first + i;
        const char* nm = names + (DWORD)idx * (DWORD)nameStride;
        DWORD c = (idx == sel) ? glow : text;
        Font_DrawText(d, inL + (idx == sel ? 12.0f : 0.0f), inT + 40.0f + rowDY * (float)i,
            nm, FONT_SIZE_SMALL, c, (int)(560.0f - inL));
    }
}

void IsoUi_Draw(IDirect3DDevice8* d) {
    float inL, inR, inT, inB;
    DWORD text, glow, dim;

    if (!s_open) return;
    if (s_state == ST_FOLDER) return;   /* dd_browse owns the screen here */

    text = Theme_Color("text", 0xFFD8F8C0);
    glow = Theme_Color("glow", 0xFFAEFF3C);
    dim = Theme_Color("text_dim", 0xFF7FA060);

    Panel(d, &inL, &inR, &inT, &inB);

    if (s_state == ST_PICK) {
        Font_DrawTextCentered(d, inL, inT, inR - inL, "Select an ISO", FONT_SIZE_MEDIUM, glow);
        DrawList(d, inL, inT, (const char*)s_isoNames, s_isoCount, s_isoSel, text, glow, dim, ISOUI_NAME_MAX);
        Font_DrawText(d, inL, inB - 4.0f, "A  INSTALL      B  CANCEL", FONT_SIZE_SMALL, dim, 0);
    }
    else if (s_state == ST_ROOT) {
        Font_DrawTextCentered(d, inL, inT, inR - inL, "Install to", FONT_SIZE_MEDIUM, glow);
        DrawList(d, inL, inT, (const char*)s_roots, s_rootCount, s_rootSel, text, glow, dim, ISOUI_PATH_MAX);
        Font_DrawText(d, inL, inB - 4.0f, "A  SELECT      B  BACK", FONT_SIZE_SMALL, dim, 0);
    }
    else if (s_state == ST_BUSY) {
        Font_DrawTextCentered(d, inL, (inT + inB) * 0.5f - 8.0f, inR - inL,
            "INSTALLING...", FONT_SIZE_MEDIUM, text);
    }
    else if (s_state == ST_RESULT) {
        DWORD c = s_resultOk ? glow : Theme_Color("warn", 0xFFFF6040);
        Font_DrawTextCentered(d, inL, (inT + inB) * 0.5f - 16.0f, inR - inL,
            ResultText(), FONT_SIZE_MEDIUM, c);
        Font_DrawTextCentered(d, inL, inB - 4.0f, inR - inL, "A  OK", FONT_SIZE_SMALL, dim);
    }
}