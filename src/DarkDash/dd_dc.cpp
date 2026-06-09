/*---------------------------------------------------------------------------
    dd_dc.cpp -- see dd_dc.h.

    Versioned shared config with SAFE migration. On load:
      - exact current version  -> use as-is
      - older known version    -> migrate up (preserve old fields, default new)
      - missing / corrupt / newer-than-we-know -> defaults
    Adding a device later means: bump DC_VER, add fields, and handle the
    older-version case in DcMigrate so existing files upgrade cleanly.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_dc.h"

#define DC_MAGIC   0x44434443UL   /* 'DCDC' */
#define DC_VER     5

/* The on-disk blob. Keep fields append-only across versions; never reorder or
   remove (that would break migration of older files). */
typedef struct {
    DWORD magic;
    DWORD version;

    /* --- v1: Type-D --- */
    int   typedEnabled;

    /* --- v2: RGB + OXFP basic --- */
    int   rgbMode;
    int   rgbBright;
    int   rgbSpeed;
    int   oxfpMode;
    int   oxfpBright;

    /* --- v3: RGB color/intensity + OXFP color/anim (colors = palette idx) --- */
    int   rgbIntensity;
    int   rgbPalCount;
    int   rgbColA, rgbColB, rgbColC, rgbColD;
    int   oxfpAnim;
    int   oxfpAnimSpeed;
    int   oxfpGreen, oxfpRed, oxfpOrange;
    int   oxfpAnimA, oxfpAnimB;

    /* --- v4: Type-D "Now Playing" cover art pushed on game launch --- */
    int   typedArtEnabled;

    /* --- v5: same art for the regular Type-D units (ids 1-4); XL is v4 --- */
    int   typedCtrlArtEnabled;

    /* future: append new fields here, bump DC_VER, default them in DcMigrate */
} DcBlob;

static DcBlob s_dc;
static int    s_loaded = 0;

#define DC_PATH "D:\\data\\dc.dat"

/* Fill a blob with all-current-version defaults. */
static void DcDefaults(DcBlob* b) {
    b->magic = DC_MAGIC;
    b->version = DC_VER;
    b->typedEnabled = 0;          /* off until the user turns it on */
    b->rgbMode = 0;
    b->rgbBright = 128;
    b->rgbSpeed = 128;
    b->oxfpMode = 0;
    b->oxfpBright = 128;
    b->rgbIntensity = 128;
    b->rgbPalCount = 2;
    b->rgbColA = 0; b->rgbColB = 1; b->rgbColC = 2; b->rgbColD = 3;
    b->oxfpAnim = 0; b->oxfpAnimSpeed = 128;
    b->oxfpGreen = 4; b->oxfpRed = 0; b->oxfpOrange = 2;
    b->oxfpAnimA = 0; b->oxfpAnimB = 8;
    b->typedArtEnabled = 0;       /* opt-in; further gated by device discovery */
    b->typedCtrlArtEnabled = 0;   /* opt-in; regular Type-D (1-4) art */
}

/* Migrate an older-but-valid blob up to the current version in place. Each
   step fills in the fields that version introduced. */
static void DcMigrate(DcBlob* b) {
    /* v1 -> v2: a v1 file had no RGB/OXFP fields. When we loaded it, the bytes
       past typedEnabled stayed at the defaults we pre-seeded (Dc_Load copies
       only the bytes present on disk over a default blob), so they're already
       sane -- we just stamp the new version. This is the safe-upgrade path:
       the user's Type-D setting carries forward, new fields get defaults. */
    if (b->version < 2) {
        /* (fields already defaulted by Dc_Load's pre-seed; nothing to copy) */
        b->version = 2;
    }
    if (b->version < 3) {
        /* v3 fields likewise already at defaults from the pre-seed */
        b->version = 3;
    }
    if (b->version < 4) {
        /* v4 field (typedArtEnabled) already at default from the pre-seed */
        b->version = 4;
    }
    if (b->version < 5) {
        /* v5 field (typedCtrlArtEnabled) already at default from the pre-seed */
        b->version = 5;
    }
    b->version = DC_VER;
    b->magic = DC_MAGIC;
}

void Dc_Load(void) {
    HANDLE h; DWORD got = 0; DcBlob tmp;

    if (s_loaded) return;
    s_loaded = 1;
    DcDefaults(&s_dc);

    h = CreateFileA(DC_PATH, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;   /* no file -> keep defaults */

    /* Read what we can. The file may be SHORTER (older, fewer fields) or the
       same size. Start from defaults so any field not present on disk keeps its
       default value -- this is the safe-upgrade guarantee. */
    {
        DWORD toRead = sizeof(tmp);
        ZeroMemory(&tmp, sizeof(tmp));
        ReadFile(h, &tmp, toRead, &got, NULL);
    }
    CloseHandle(h);

    /* Validate header. Accept equal or older versions; migrate. Reject unknown
       magic or a version newer than we understand (keep defaults). */
    if (got >= (DWORD)(sizeof(DWORD) * 2) &&
        tmp.magic == DC_MAGIC &&
        tmp.version >= 1 && tmp.version <= DC_VER) {

        /* copy the bytes we actually read over the default blob, so present
           fields override defaults and absent (newer) fields stay default */
        BYTE* dst = (BYTE*)&s_dc;
        BYTE* src = (BYTE*)&tmp;
        DWORD i;
        for (i = 0; i < got && i < (DWORD)sizeof(s_dc); i++) dst[i] = src[i];

        DcMigrate(&s_dc);
    }
    /* else: corrupt / foreign / too-new -> defaults already in place */
}

void Dc_Save(void) {
    HANDLE h; DWORD wr = 0;
    if (!s_loaded) Dc_Load();
    s_dc.magic = DC_MAGIC;
    s_dc.version = DC_VER;
    CreateDirectoryA("D:\\data", NULL);
    h = CreateFileA(DC_PATH, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, &s_dc, sizeof(s_dc), &wr, NULL);
    CloseHandle(h);
}

int  Dc_TypeDEnabled(void) { if (!s_loaded) Dc_Load(); return s_dc.typedEnabled; }
void Dc_SetTypeDEnabled(int on) {
    if (!s_loaded) Dc_Load();
    s_dc.typedEnabled = on ? 1 : 0;
    Dc_Save();
}

int  Dc_TypeDArtEnabled(void) { if (!s_loaded) Dc_Load(); return s_dc.typedArtEnabled; }
void Dc_SetTypeDArtEnabled(int on) {
    if (!s_loaded) Dc_Load();
    s_dc.typedArtEnabled = on ? 1 : 0;
    Dc_Save();
}

int  Dc_TypeDCtrlArtEnabled(void) { if (!s_loaded) Dc_Load(); return s_dc.typedCtrlArtEnabled; }
void Dc_SetTypeDCtrlArtEnabled(int on) {
    if (!s_loaded) Dc_Load();
    s_dc.typedCtrlArtEnabled = on ? 1 : 0;
    Dc_Save();
}

int  Dc_RgbMode(void) { if (!s_loaded) Dc_Load(); return s_dc.rgbMode; }
int  Dc_RgbBright(void) { if (!s_loaded) Dc_Load(); return s_dc.rgbBright; }
int  Dc_RgbSpeed(void) { if (!s_loaded) Dc_Load(); return s_dc.rgbSpeed; }
void Dc_SetRgbMode(int m) { if (!s_loaded) Dc_Load(); s_dc.rgbMode = m;   Dc_Save(); }
void Dc_SetRgbBright(int b) { if (!s_loaded) Dc_Load(); s_dc.rgbBright = b; Dc_Save(); }
void Dc_SetRgbSpeed(int s) { if (!s_loaded) Dc_Load(); s_dc.rgbSpeed = s;  Dc_Save(); }

int  Dc_OxfpMode(void) { if (!s_loaded) Dc_Load(); return s_dc.oxfpMode; }
int  Dc_OxfpBright(void) { if (!s_loaded) Dc_Load(); return s_dc.oxfpBright; }
void Dc_SetOxfpMode(int m) { if (!s_loaded) Dc_Load(); s_dc.oxfpMode = m;   Dc_Save(); }
void Dc_SetOxfpBright(int b) { if (!s_loaded) Dc_Load(); s_dc.oxfpBright = b; Dc_Save(); }

void Dc_SaveRgb(int mode, int bright, int speed) {
    if (!s_loaded) Dc_Load();
    s_dc.rgbMode = mode; s_dc.rgbBright = bright; s_dc.rgbSpeed = speed;
    Dc_Save();
}
void Dc_SaveOxfp(int mode, int bright) {
    if (!s_loaded) Dc_Load();
    s_dc.oxfpMode = mode; s_dc.oxfpBright = bright;
    Dc_Save();
}

void Dc_SaveRgb2(int mode, int bright, int speed, int intensity, int palCount,
    int colA, int colB, int colC, int colD) {
    if (!s_loaded) Dc_Load();
    s_dc.rgbMode = mode; s_dc.rgbBright = bright; s_dc.rgbSpeed = speed;
    s_dc.rgbIntensity = intensity; s_dc.rgbPalCount = palCount;
    s_dc.rgbColA = colA; s_dc.rgbColB = colB; s_dc.rgbColC = colC; s_dc.rgbColD = colD;
    Dc_Save();
}
void Dc_SaveOxfp2(int mode, int bright, int anim, int animSpeed,
    int green, int red, int orange, int animA, int animB) {
    if (!s_loaded) Dc_Load();
    s_dc.oxfpMode = mode; s_dc.oxfpBright = bright;
    s_dc.oxfpAnim = anim; s_dc.oxfpAnimSpeed = animSpeed;
    s_dc.oxfpGreen = green; s_dc.oxfpRed = red; s_dc.oxfpOrange = orange;
    s_dc.oxfpAnimA = animA; s_dc.oxfpAnimB = animB;
    Dc_Save();
}

int Dc_RgbIntensity(void) { if (!s_loaded) Dc_Load(); return s_dc.rgbIntensity; }
int Dc_RgbPalCount(void) { if (!s_loaded) Dc_Load(); return s_dc.rgbPalCount; }
int Dc_RgbColor(int slot) {
    if (!s_loaded) Dc_Load();
    switch (slot) {
    case 0: return s_dc.rgbColA; case 1: return s_dc.rgbColB;
    case 2: return s_dc.rgbColC; default: return s_dc.rgbColD;
    }
}
int Dc_OxfpAnim(void) { if (!s_loaded) Dc_Load(); return s_dc.oxfpAnim; }
int Dc_OxfpAnimSpeed(void) { if (!s_loaded) Dc_Load(); return s_dc.oxfpAnimSpeed; }
int Dc_OxfpStatus(int which) {
    if (!s_loaded) Dc_Load();
    switch (which) {
    case 0: return s_dc.oxfpGreen; case 1: return s_dc.oxfpRed;
    default: return s_dc.oxfpOrange;
    }
}
int Dc_OxfpAnimColor(int ab) { if (!s_loaded) Dc_Load(); return ab ? s_dc.oxfpAnimB : s_dc.oxfpAnimA; }