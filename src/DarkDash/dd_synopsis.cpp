/*---------------------------------------------------------------------------
    dd_synopsis.cpp -- see dd_synopsis.h.

    Reads the flat <tag> fields from _resources\default.xml and loads
    artwork\poster.jpg, then lays them out in a themed overlay: box art on the
    left, title + key facts + a scrolling overview on the right. C89 style, no
    CRT str*, file-scope statics.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_synopsis.h"
#include "dd_ui.h"
#include "dd_theme.h"
#include "dd_texture.h"
#include "font.h"
#include "input.h"
#include "dd_audio.h"

#define SYN_PATH_MAX   260
#define SYN_OVR_MAX    1024
#define SYN_FLD_MAX    96

static int      s_open = 0;
static Texture  s_poster;                 /* front cover (poster.jpg)        */
static Texture  s_back;                   /* back cover  (alt_synopsis.jpg)  */
static int      s_havePoster = 0;
static int      s_haveBack = 0;
static int      s_view = 0;               /* 0 = front, 1 = back             */
static float    s_scroll = 0.0f;          /* overview scroll offset (px)     */
static float    s_scrollMax = 0.0f;       /* clamp, computed during draw     */

/* parsed fields */
static char s_title[SYN_FLD_MAX];
static char s_dev[SYN_FLD_MAX];
static char s_pub[SYN_FLD_MAX];
static char s_genre[SYN_FLD_MAX];
static char s_esrb[SYN_FLD_MAX];
static char s_year[SYN_FLD_MAX];
static char s_rating[SYN_FLD_MAX];
static char s_overview[SYN_OVR_MAX];

/* ---- tiny string + path helpers ---------------------------------------- */

static void SCopy(char* d, int cap, const char* s) {
    int i = 0; if (cap <= 0) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void TitleFolder(const char* xbePath, char* out, int cap) {
    int n = 0, last = -1, i;
    while (xbePath[n] && n < cap - 1) { out[n] = xbePath[n]; n++; }
    out[n] = 0;
    for (i = 0; i < n; i++) if (out[i] == '\\') last = i;
    if (last >= 0) out[last] = 0;     /* drop "\default.xbe" */
}

static void JoinPath(char* out, int cap, const char* a, const char* b) {
    int n = 0, i = 0;
    while (a[n] && n < cap - 1) { out[n] = a[n]; n++; }
    if (n > 0 && out[n - 1] != '\\' && n < cap - 1) out[n++] = '\\';
    while (b[i] && n < cap - 1) out[n++] = b[i++];
    out[n] = 0;
}

/* copy text between <tag> and </tag>; trims surrounding whitespace. 1 if found. */
static int XmlTag(const char* xml, const char* tag, char* out, int cap) {
    char open[40], close[40];
    int oi = 0, ci = 0, i, n;
    const char* p; const char* s = 0;
    open[oi++] = '<'; for (i = 0; tag[i] && oi < 38; i++) open[oi++] = tag[i]; open[oi++] = '>'; open[oi] = 0;
    close[ci++] = '<'; close[ci++] = '/'; for (i = 0; tag[i] && ci < 38; i++) close[ci++] = tag[i]; close[ci++] = '>'; close[ci] = 0;

    for (p = xml; *p; p++) {
        const char* a = p; const char* b = open; int m = 1;
        while (*b) { if (*a != *b) { m = 0; break; } a++; b++; }
        if (m) { s = a; break; }
    }
    if (!s) { if (cap > 0) out[0] = 0; return 0; }

    n = 0;
    {
        const char* q = s;
        while (*q && n < cap - 1) {
            const char* a = q; const char* b = close; int m = 1;
            while (*b) { if (*a != *b) { m = 0; break; } a++; b++; }
            if (m) break;
            out[n++] = *q++;
        }
    }
    out[n] = 0;
    while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == '\n' || out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = 0;
    /* also trim leading whitespace */
    {
        int lead = 0;
        while (out[lead] == '\r' || out[lead] == '\n' || out[lead] == ' ' || out[lead] == '\t') lead++;
        if (lead > 0) { int k = 0; while (out[lead]) out[k++] = out[lead++]; out[k] = 0; }
    }
    return 1;
}

/* read the whole default.xml into buf (bounded). bytes read, 0 on failure. */
static int ReadXml(const char* xbePath, char* buf, int cap) {
    char folder[SYN_PATH_MAX], path[SYN_PATH_MAX];
    HANDLE h; DWORD got = 0;
    TitleFolder(xbePath, folder, sizeof(folder));
    JoinPath(path, sizeof(path), folder, "_resources\\default.xml");
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ReadFile(h, buf, (DWORD)(cap - 1), &got, NULL);
    CloseHandle(h);
    if (got == 0) return 0;
    buf[got] = 0;
    return (int)got;
}

/* ---- public ------------------------------------------------------------ */

int Synopsis_Available(const char* xbePath) {
    char folder[SYN_PATH_MAX], path[SYN_PATH_MAX];
    DWORD attr;
    if (!xbePath || !xbePath[0]) return 0;
    TitleFolder(xbePath, folder, sizeof(folder));
    JoinPath(path, sizeof(path), folder, "_resources\\default.xml");
    attr = GetFileAttributesA(path);
    return (attr != 0xFFFFFFFF && !(attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

void Synopsis_Open(const char* xbePath) {
    char folder[SYN_PATH_MAX], path[SYN_PATH_MAX];
    char xml[2048];
    int  got;

    Synopsis_Close();
    if (!xbePath || !xbePath[0]) return;

    s_title[0] = s_dev[0] = s_pub[0] = s_genre[0] = s_esrb[0] = s_year[0] = s_rating[0] = s_overview[0] = 0;

    got = ReadXml(xbePath, xml, sizeof(xml));
    if (got > 0) {
        XmlTag(xml, "title", s_title, sizeof(s_title));
        XmlTag(xml, "developer", s_dev, sizeof(s_dev));
        XmlTag(xml, "publisher", s_pub, sizeof(s_pub));
        XmlTag(xml, "genre", s_genre, sizeof(s_genre));
        XmlTag(xml, "esrb", s_esrb, sizeof(s_esrb));
        XmlTag(xml, "year", s_year, sizeof(s_year));
        XmlTag(xml, "rating", s_rating, sizeof(s_rating));
        XmlTag(xml, "overview", s_overview, sizeof(s_overview));
    }
    if (!s_title[0]) SCopy(s_title, sizeof(s_title), "(untitled)");

    /* Load the box art. XBMC4Gamers packs ship JPG (poster.jpg / alt_synopsis.jpg),
       usually progressive -- which stb_image (dd_stbi.c) now decodes natively, so
       no conversion is needed. We still try the JPG first, then fall back to a PNG
       of the same name (poster.png / alt_synopsis.png); stb handles both. */
    TitleFolder(xbePath, folder, sizeof(folder));
    JoinPath(path, sizeof(path), folder, "_resources\\artwork\\poster.jpg");
    s_havePoster = Texture_LoadJPEG(path, &s_poster) ? 1 : 0;
    if (!s_havePoster) {
        JoinPath(path, sizeof(path), folder, "_resources\\artwork\\poster.png");
        s_havePoster = Texture_LoadPNG(path, &s_poster) ? 1 : 0;
    }

    JoinPath(path, sizeof(path), folder, "_resources\\artwork\\alt_synopsis.jpg");
    s_haveBack = Texture_LoadJPEG(path, &s_back) ? 1 : 0;
    if (!s_haveBack) {
        JoinPath(path, sizeof(path), folder, "_resources\\artwork\\alt_synopsis.png");
        s_haveBack = Texture_LoadPNG(path, &s_back) ? 1 : 0;
    }

    s_view = 0;
    s_scroll = 0.0f;
    s_scrollMax = 0.0f;
    s_open = 1;
}

void Synopsis_Close(void) {
    if (s_havePoster) { Texture_Release(&s_poster); s_havePoster = 0; }
    if (s_haveBack) { Texture_Release(&s_back);   s_haveBack = 0; }
    s_poster.tex = NULL;
    s_back.tex = NULL;
    s_open = 0;
}

int Synopsis_IsOpen(void) { return s_open; }

int Synopsis_Update(WORD pressed) {
    int lx, ly, rx, ry;
    if (!s_open) return 1;
    if (pressed & (BTN_B | BTN_BACK | BTN_WHITE)) {
        Audio_PlaySfx(SFX_BACK);
        Synopsis_Close();
        return 1;
    }

    /* D-pad left/right toggles front <-> back cover */
    if (pressed & BTN_DPAD_LEFT) { if (s_view != 0) { s_view = 0; Audio_PlaySfx(SFX_ALT); } }
    if (pressed & BTN_DPAD_RIGHT) { if (s_view != 1 && s_haveBack) { s_view = 1; Audio_PlaySfx(SFX_ALT); } }

    /* text scroll: analog stick (smooth, proportional) + D-pad up/down (step).
       Use whichever stick is being pushed (left preferred, right as fallback) so
       it responds regardless of which one the user reaches for, and scale the
       speed by deflection so a gentle push creeps and a full push flies. */
    GetSticks(lx, ly, rx, ry);
    {
        int v = (ly != 0) ? ly : ry;        /* +up / -down, deadzoned upstream */
        if (v != 0) s_scroll -= (float)v / 2600.0f;   /* up -> toward top */
    }
    if (pressed & BTN_DPAD_DOWN) { s_scroll += 24.0f; Audio_PlaySfx(SFX_NAV_DOWN); }
    if (pressed & BTN_DPAD_UP) { s_scroll -= 24.0f; Audio_PlaySfx(SFX_NAV_UP); }

    if (s_scroll < 0.0f)        s_scroll = 0.0f;
    if (s_scroll > s_scrollMax) s_scroll = s_scrollMax;
    return 0;
}

/* draw one "Label  value" fact line clipped to maxRight; returns next y */
static float FactLine(IDirect3DDevice8* d, float x, float y, float labelW,
    const char* label, const char* value, float maxRight,
    DWORD lc, DWORD vc) {
    if (!value || !value[0]) return y;
    Font_DrawText(d, x, y, label, FONT_SIZE_SMALL, lc, 0);
    Font_DrawText(d, x + labelW, y, value, FONT_SIZE_SMALL, vc,
        (int)(maxRight - (x + labelW)));
    return y + 20.0f;
}

/* Wrap+draw the overview inside [ovX,ovY,ovW] starting at (ovY - scroll). When
   draw==0, only measures (no drawing) to compute total height. Returns the
   total laid-out height in px. */
static float OverviewLayout(IDirect3DDevice8* d, float ovX, float ovY, float ovW,
    float scroll, DWORD col, int draw) {
    char  line[160];
    char  word[64];
    int   li = 0, i = 0, wl, k, t;
    float lineH = (float)Font_LineHeight(FONT_SIZE_SMALL);
    float y = ovY - scroll;
    float used = 0.0f;
    char  trial[160];

    line[0] = 0;
    while (s_overview[i]) {
        while (s_overview[i] == ' ' || s_overview[i] == '\r') i++;
        if (s_overview[i] == '\n') {
            if (draw) Font_DrawText(d, ovX, y, line, FONT_SIZE_SMALL, col, 0);
            y += lineH; used += lineH; line[0] = 0; li = 0; i++; continue;
        }
        wl = 0;
        while (s_overview[i] && s_overview[i] != ' ' &&
            s_overview[i] != '\n' && s_overview[i] != '\r' &&
            wl < (int)sizeof(word) - 1) {
            word[wl++] = s_overview[i++];
        }
        word[wl] = 0;
        if (wl == 0) continue;

        t = 0;
        for (k = 0; line[k] && t < (int)sizeof(trial) - 2; k++) trial[t++] = line[k];
        if (li > 0 && t < (int)sizeof(trial) - 2) trial[t++] = ' ';
        for (k = 0; word[k] && t < (int)sizeof(trial) - 1; k++) trial[t++] = word[k];
        trial[t] = 0;

        if (li > 0 && (float)Font_MeasureText(trial, FONT_SIZE_SMALL) > ovW) {
            if (draw) Font_DrawText(d, ovX, y, line, FONT_SIZE_SMALL, col, 0);
            y += lineH; used += lineH;
            SCopy(line, sizeof(line), word); li = wl;
        }
        else {
            SCopy(line, sizeof(line), trial); li = t;
        }
    }
    if (li > 0) { if (draw) Font_DrawText(d, ovX, y, line, FONT_SIZE_SMALL, col, 0); used += lineH; }
    return used;
}

void Synopsis_Draw(IDirect3DDevice8* d) {
    const Texture* frame;
    DWORD text, glow, dim, accent;
    /* frame_menu_v has thick chrome: ~34px top, ~26px bottom. The asset is ~272
       wide but we stretch it to 560 here, so its side borders render ~2x wider
       than in dd_browse -- the old 24px side inset sat inside that chrome, which
       jammed the artwork and footer against the border. Use a wider side inset
       so content clears the stretched chrome. */
    float fx = 40.0f, fy = 48.0f, fw = 560.0f, fh = 384.0f;
    float inL = fx + 32.0f;                 /* left interior edge   */
    float inR = fx + fw - 32.0f;            /* right interior edge  */
    float inT = fy + 34.0f;                 /* top interior edge    */
    float inB = fy + fh - 26.0f;            /* bottom interior edge */
    float px, py, pw, ph;                   /* image rect (left)    */
    float rx, ry, rRight;                   /* text column (right)  */
    float labelW;                           /* fact-label column width */
    const Texture* img;
    int   haveImg;

    if (!s_open) return;

    text = Theme_Color("text", 0xFFD8F8C0);
    glow = Theme_Color("glow", 0xFFAEFF3C);
    accent = Theme_Color("accent", 0xFF7FE000);
    dim = Theme_Color("text_dim", 0xFF7FA060);
    frame = Theme_Asset("frame_menu_v");

    {
        DWORD bg = Theme_Color("bg", 0xFF060A08);
        int br = (int)((bg >> 16) & 0xFF), bgg = (int)((bg >> 8) & 0xFF), bb = (int)(bg & 0xFF);
        UI_FillRect(0.0f, 0.0f, 640.0f, 480.0f, UI_ARGB(180, br / 3, bgg / 3, bb / 3));
    }
    if (frame) UI_DrawSprite(frame, fx, fy, fw, fh, 0xFFFFFFFF, 0);

    /* ---- left: cover image (front poster.jpg / back alt_synopsis.jpg) ---- */
    /* both pack images are 362x512-ish portrait; size to fit the left band
       inside the interior with margin, preserving that aspect. */
    pw = 170.0f;
    ph = pw * 512.0f / 362.0f;              /* ~240px, portrait              */
    px = inL + 8.0f;                        /* leftmost spot that still clears the chrome (no butting) */
    py = inT + 6.0f;
    if (py + ph > inB - 16.0f) ph = (inB - 16.0f) - py;   /* never exceed interior */

    img = (s_view == 1) ? &s_back : &s_poster;
    haveImg = (s_view == 1) ? s_haveBack : s_havePoster;
    if (haveImg && img->tex) {
        UI_DrawSprite(img, px, py, pw, ph, 0xFFFFFFFF, 0);
    }
    else {
        DWORD bg = Theme_Color("bg", 0xFF060A08);
        int br = (int)((bg >> 16) & 0xFF), bgg = (int)((bg >> 8) & 0xFF), bb = (int)(bg & 0xFF);
        UI_FillRect(px, py, pw, ph, UI_ARGB(90, br * 70 / 100, bgg * 70 / 100, bb * 70 / 100));
        Font_DrawTextCentered(d, px + pw * 0.5f, py + ph * 0.5f - 8.0f, pw,
            s_view == 1 ? "No Back Art" : "No Art", FONT_SIZE_SMALL, dim);
    }

    /* front/back indicator + hint under the image (inside interior) */
    {
        const char* vlabel = (s_view == 1) ? "Back" : "Front";
        Font_DrawTextCentered(d, px + pw * 0.5f, py + ph + 4.0f, pw, vlabel, FONT_SIZE_SMALL, glow);
        if (s_haveBack)
            Font_DrawTextCentered(d, px + pw * 0.5f, py + ph + 22.0f, pw,
                "< L/R >", FONT_SIZE_SMALL, dim);
    }

    /* ---- right: formatted synopsis text, all clipped to rRight ---------- */
    rx = px + pw + 24.0f;
    rRight = inR;
    ry = inT + 2.0f;

    Font_DrawText(d, rx, ry, s_title[0] ? s_title : "(untitled)", FONT_SIZE_MEDIUM, accent,
        (int)(rRight - rx));
    ry += 28.0f;

    /* label column width: widest fact label + a small gap, so the value column
       always clears the label instead of overlapping it (the old fixed 74px was
       narrower than "Developer"/"Publisher" render at, so values rode into the
       label text). */
    {
        int wDev = Font_MeasureText("Developer", FONT_SIZE_SMALL);
        int wPub = Font_MeasureText("Publisher", FONT_SIZE_SMALL);
        int wMax = (wDev > wPub) ? wDev : wPub;
        labelW = (float)wMax + 12.0f;
    }

    ry = FactLine(d, rx, ry, labelW, "Developer", s_dev, rRight, dim, text);
    ry = FactLine(d, rx, ry, labelW, "Publisher", s_pub, rRight, dim, text);
    ry = FactLine(d, rx, ry, labelW, "Genre", s_genre, rRight, dim, text);
    {
        char line[SYN_FLD_MAX];
        int n = 0, i;
        line[0] = 0;
        if (s_esrb[0]) { const char* L = "ESRB "; for (i = 0; L[i] && n < SYN_FLD_MAX - 2; i++)line[n++] = L[i]; for (i = 0; s_esrb[i] && n < SYN_FLD_MAX - 2; i++)line[n++] = s_esrb[i]; }
        if (s_year[0]) { if (n && n < SYN_FLD_MAX - 3) { line[n++] = ' '; line[n++] = ' '; } { const char* L = "Year "; for (i = 0; L[i] && n < SYN_FLD_MAX - 2; i++)line[n++] = L[i]; } for (i = 0; s_year[i] && n < SYN_FLD_MAX - 2; i++)line[n++] = s_year[i]; }
        if (s_rating[0]) { if (n && n < SYN_FLD_MAX - 3) { line[n++] = ' '; line[n++] = ' '; } { const char* L = "Rating "; for (i = 0; L[i] && n < SYN_FLD_MAX - 2; i++)line[n++] = L[i]; } for (i = 0; s_rating[i] && n < SYN_FLD_MAX - 2; i++)line[n++] = s_rating[i]; }
        line[n] = 0;
        if (n > 0) { Font_DrawText(d, rx, ry, line, FONT_SIZE_SMALL, glow, (int)(rRight - rx)); ry += 24.0f; }
    }

    /* overview: word-wrapped + scrollable, viewport-clipped to the right
       column's lower area so it can never paint outside the frame. */
    {
        float ovX = rx;
        float ovY = ry + 4.0f;
        float ovW = rRight - rx;
        float ovBottom = inB - 20.0f;        /* leave room for footer hint    */
        float boxH = ovBottom - ovY;
        D3DVIEWPORT8 vpOld, vpClip;

        if (s_overview[0] && boxH > 8.0f) {
            float total;
            /* measure first to clamp the scroll range */
            total = OverviewLayout(d, ovX, ovY, ovW, 0.0f, text, 0);
            s_scrollMax = (total > boxH) ? (total - boxH) : 0.0f;
            if (s_scroll > s_scrollMax) s_scroll = s_scrollMax;

            d->GetViewport(&vpOld);
            vpClip.X = (DWORD)UI_Sx(ovX); vpClip.Y = (DWORD)UI_Sy(ovY);
            vpClip.Width = (DWORD)UI_ScaleX(ovW);
            vpClip.Height = (DWORD)UI_ScaleY(boxH);
            vpClip.MinZ = 0.0f; vpClip.MaxZ = 1.0f;
            d->SetViewport(&vpClip);
            OverviewLayout(d, ovX, ovY, ovW, s_scroll, text, 1);
            d->SetViewport(&vpOld);
        }
        else {
            s_scrollMax = 0.0f;
        }
    }

    /* footer hint -- centered in the frame interior. Font_DrawTextCentered takes
       the box's LEFT edge (cx) + width, so cx=inL centers it across [inL,inR]. */
    Font_DrawTextCentered(d, inL, inB - 2.0f, inR - inL,
        s_haveBack ? "L/R FRONT/BACK   STICK/UP-DN SCROLL   B CLOSE"
        : "STICK/UP-DN SCROLL   B CLOSE",
        FONT_SIZE_SMALL, dim);
}