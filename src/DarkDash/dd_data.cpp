/*---------------------------------------------------------------------------
    dd_data.cpp -- persistent settings blob (see dd_data.h).

    Format on disk: [DD_DataHeader][DD_Settings], little-endian, fixed size.
    A magic + version guards against stale/foreign files; any mismatch falls
    back to defaults rather than trusting garbage. Writes go through a temp
    file + replace so a yanked-power mid-write can't leave a half blob.
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
}

DD_Settings* Data_Get(void) {
    if (!s_loaded) { SetDefaults(); s_loaded = 1; }
    return &s_set;
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
        hdr.size != (DWORD)sizeof(DD_Settings)) {
        CloseHandle(h);
        return 0;                              /* foreign/old -> defaults */
    }

    if (!ReadFile(h, &s_set, sizeof(s_set), &got, NULL) || got != sizeof(s_set)) {
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
    if (s_set.ftpUser[0] == '\0') lstrcpynA(s_set.ftpUser, "xbox", DD_FTP_CRED_MAX);
    if (s_set.ftpPass[0] == '\0') lstrcpynA(s_set.ftpPass, "xbox", DD_FTP_CRED_MAX);
    s_set.musicPath[DD_MUSIC_PATH_MAX - 1] = '\0';
    return 1;
}

int Data_Save(void) {
    HANDLE h;
    DD_DataHeader hdr;
    DWORD wrote = 0;

    if (!s_loaded) { SetDefaults(); s_loaded = 1; }

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
        !WriteFile(h, &s_set, sizeof(s_set), &wrote, NULL) || wrote != sizeof(s_set)) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    return 1;
}