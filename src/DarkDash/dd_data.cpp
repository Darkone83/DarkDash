/*---------------------------------------------------------------------------
    dd_data.cpp -- persistent settings blob (see dd_data.h).

    Format on disk: [DD_DataHeader][DD_Settings], little-endian, fixed size.
    A magic + version guards against stale/foreign files. Older same-version
    blobs with a shorter DD_Settings prefix are migrated forward by reading the
    fields they contain over current defaults, so newly appended preferences do
    not wipe existing user choices. Writes go straight to the target because
    MoveFile/replace is unreliable on the Xbox filesystem.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <string.h>
#include "dd_data.h"

#define DD_DATA_DIR   "D:\\data"
#define DD_DATA_FILE  "D:\\data\\settings.dat"
#define DD_DATA_MAGIC 0x44445354u   /* 'DDST' */
#define DD_DATA_VER   1

typedef struct DD_DataHeader {
    DWORD magic;
    DWORD version;
    DWORD size;       /* sizeof(DD_Settings) the file was written with */
} DD_DataHeader;

static DD_Settings s_set;
static int         s_loaded = 0;

static void SetDefaults(void) {
    memset(&s_set, 0, sizeof(s_set));
    s_set.musicVolume = 70;
    s_set.musicCustom = 0;
    s_set.musicPath[0] = '\0';
    s_set.ftpEnabled = 1;   /* on by default -- the lifeline from DVD/xemu where
                                we can't persist or easily launch other tools */
    s_set.ftpPort = 21;
    lstrcpynA(s_set.ftpUser, "xbox", DD_FTP_CRED_MAX);
    lstrcpynA(s_set.ftpPass, "xbox", DD_FTP_CRED_MAX);
    s_set.fanAuto = 1;
    s_set.fanPercent = 50;
    s_set.videoAspect = DD_VIDEO_PILLARBOX;  /* default: undistorted */
    s_set.videoRes = DD_RES_AUTO;         /* follow the console's setting */
    s_set.fontName[0] = '\0';                /* baked Default failsafe */
    s_set.themeName[0] = '\0';               /* default theme */
    s_set.fxFlags = DD_FX_DEFAULT;       /* all character effects on */
    s_set.lightingPct = 100;
    s_set.shadowPct = 100;
    s_set.reflectionPct = 100;
    s_set.atmospherePct = 100;
    s_set.parallaxPct = 100;
    s_set.screensaverMin = 10;               /* idle screensaver after 10 min */
}

DD_Settings* Data_Get(void) {
    if (!s_loaded) { SetDefaults(); s_loaded = 1; }
    return &s_set;
}

int Data_FxOn(int bit) {
    if (!s_loaded) { SetDefaults(); s_loaded = 1; }
    return (s_set.fxFlags & bit) ? 1 : 0;
}

int Data_EffectPercent(int effect) {
    int value = 100;
    DD_Settings* st = Data_Get();
    switch (effect) {
    case DD_EFFECT_LIGHTING: value = st->lightingPct; break;
    case DD_EFFECT_SHADOW: value = st->shadowPct; break;
    case DD_EFFECT_REFLECTION: value = st->reflectionPct; break;
    case DD_EFFECT_ATMOSPHERE: value = st->atmospherePct; break;
    case DD_EFFECT_PARALLAX: value = st->parallaxPct; break;
    }
    if (value < 0) value = 0;
    if (value > 200) value = 200;
    return value;
}

int Data_EffectScale(int effect, int alpha) {
    int value = alpha * Data_EffectPercent(effect) / 100;
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

int Data_Load(void) {
    HANDLE h;
    DD_DataHeader hdr;
    DWORD got = 0;

    SetDefaults();
    s_loaded = 1;

    h = CreateFileA(DD_DATA_FILE, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;   /* no file -> defaults */

    if (!ReadFile(h, &hdr, sizeof(hdr), &got, NULL) || got != sizeof(hdr) ||
        hdr.magic != DD_DATA_MAGIC || hdr.version != DD_DATA_VER ||
        hdr.size == 0 || hdr.size > (DWORD)sizeof(DD_Settings)) {
        CloseHandle(h);
        return 0;                              /* foreign/old -> defaults */
    }

    /* SetDefaults() already populated the full current struct. Reading only the
       older prefix preserves all known user fields while leaving newly-added
       tail fields at their modern defaults. */
    if (!ReadFile(h, &s_set, hdr.size, &got, NULL) || got != hdr.size) {
        CloseHandle(h);
        SetDefaults();                         /* truncated -> defaults */
        return 0;
    }
    CloseHandle(h);

    /* clamp anything that could have been hand-edited out of range */
    if (s_set.musicVolume < 0)   s_set.musicVolume = 0;
    if (s_set.musicVolume > 100) s_set.musicVolume = 100;
    if (s_set.fanPercent < 0)   s_set.fanPercent = 0;
    if (s_set.fanPercent > 100) s_set.fanPercent = 100;
    if (s_set.ftpPort < 1 || s_set.ftpPort > 65535) s_set.ftpPort = 21;
    /* A shorter migrated blob can end part-way through a string field. Always
       terminate persisted strings before any UI/service code consumes them. */
    s_set.musicPath[DD_MUSIC_PATH_MAX - 1] = '\0';
    s_set.ftpUser[DD_FTP_CRED_MAX - 1] = '\0';
    s_set.ftpPass[DD_FTP_CRED_MAX - 1] = '\0';
    s_set.fontName[sizeof(s_set.fontName) - 1] = '\0';
    s_set.themeName[sizeof(s_set.themeName) - 1] = '\0';
    if (s_set.ftpUser[0] == '\0') lstrcpynA(s_set.ftpUser, "xbox", DD_FTP_CRED_MAX);
    if (s_set.ftpPass[0] == '\0') lstrcpynA(s_set.ftpPass, "xbox", DD_FTP_CRED_MAX);
    if (s_set.lightingPct < 0 || s_set.lightingPct > 200) s_set.lightingPct = 100;
    if (s_set.shadowPct < 0 || s_set.shadowPct > 200) s_set.shadowPct = 100;
    if (s_set.reflectionPct < 0 || s_set.reflectionPct > 200) s_set.reflectionPct = 100;
    if (s_set.atmospherePct < 0 || s_set.atmospherePct > 200) s_set.atmospherePct = 100;
    if (s_set.parallaxPct < 0 || s_set.parallaxPct > 200) s_set.parallaxPct = 100;
    if (s_set.musicMode < DD_MUSIC_NORMAL || s_set.musicMode > DD_MUSIC_SHUFFLE)
        s_set.musicMode = DD_MUSIC_NORMAL;
    /* fxFlags carries a DD_FX_SET sentinel that Data_Save always sets. If it's
       absent, the value is a legacy/zeroed blob that predates fxFlags -> default
       to all-on. If it's present, honour the stored bits EXACTLY -- including the
       user turning every effect off (stored as DD_FX_SET alone). */
    if (!(s_set.fxFlags & DD_FX_SET)) {
        s_set.fxFlags = DD_FX_DEFAULT;
    }
    else {
        s_set.fxFlags &= DD_FX_ALL;   /* strip the sentinel for in-memory use */
    }
    return 1;
}

int Data_Save(void) {
    HANDLE h;
    DD_DataHeader hdr;
    DD_Settings   blob;
    DWORD wrote = 0;

    if (!s_loaded) { SetDefaults(); s_loaded = 1; }

    /* On-disk copy carries the DD_FX_SET sentinel so a stored value of 0 is
       always identifiable as legacy (never a deliberate all-off). The live
       s_set stays sentinel-free. */
    blob = s_set;
    blob.fxFlags = (s_set.fxFlags & DD_FX_ALL) | DD_FX_SET;

    /* create D:\data if it isn't there yet (ignore "already exists") */
    CreateDirectoryA(DD_DATA_DIR, NULL);

    /* write straight to the target, XbDiag-style: CREATE_ALWAYS truncates or
       creates. No temp+rename -- MoveFile is unreliable on the Xbox FS. */
    h = CreateFileA(DD_DATA_FILE, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;   /* read-only volume -> give up quietly */

    hdr.magic = DD_DATA_MAGIC; hdr.version = DD_DATA_VER;
    hdr.size = (DWORD)sizeof(DD_Settings);

    if (!WriteFile(h, &hdr, sizeof(hdr), &wrote, NULL) || wrote != sizeof(hdr) ||
        !WriteFile(h, &blob, sizeof(blob), &wrote, NULL) || wrote != sizeof(blob)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    return 1;
}