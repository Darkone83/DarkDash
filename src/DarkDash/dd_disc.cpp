/*---------------------------------------------------------------------------
    dd_disc.cpp -- optical disc monitor (see dd_disc.h).

    SMC tray states (from xboxinternals.h):
        SMC_TRAY_STATE_CLOSED      0x00  (tray shut; media seated when present)
        SMC_TRAY_STATE_OPEN        0x10
        SMC_TRAY_STATE_NO_MEDIA    0x40
        ...transient: OPENING/CLOSING/UNLOADING/MEDIA_DETECT/RESET

    We treat a transition INTO "closed" as "media present" and mount once; a
    transition into OPEN / NO_MEDIA clears the mount. Cdrom0 is remapped to Q:
    (a staging letter) so the dashboard's own D: assets stay valid.

    Build: MSVC2003/C89 style; file-scope statics; no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"
#include "dd_disc.h"
#include "dd_xbe.h"
#include "dd_mount.h"

#define DISC_DEVICE  "\\Device\\Cdrom0"
#define DISC_LETTER  'Q'

static DiscState s_disc;
static int       s_mounted = 0;     /* Q: currently symlinked? */
static ULONG     s_lastTray = 0xFFFFFFFF;
static int       s_haveLast = 0;

typedef STRING XBOX_STRING;

/* ---- helpers ---------------------------------------------------------- */

static int DcLen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void DcCopy(char* d, int cap, const char* s) {
    int i = 0; if (cap <= 0) return;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0;
}

/* remap Cdrom0 -> Q: (dismount first so a fresh disc is re-read) */
static void MountDisc(void) {
    char linkBuf[8];
    const char* dev = DISC_DEVICE;
    int devLen = DcLen(dev);
    XBOX_STRING sLink, sDev, sName;
    char namebuf[8];

    /* dismount Cdrom0 by name so the kernel re-reads the new media */
    namebuf[0] = '\\'; namebuf[1] = '?'; namebuf[2] = '?'; namebuf[3] = '\\';
    namebuf[4] = DISC_LETTER; namebuf[5] = ':'; namebuf[6] = 0;

    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = DISC_LETTER; linkBuf[5] = ':'; linkBuf[6] = 0;

    /* drop any prior Q: link, then (re)create it pointing at Cdrom0 */
    sName.Length = 6; sName.MaximumLength = 7; sName.Buffer = namebuf;
    IoDeleteSymbolicLink(&sName);   /* ignore error if not present */

    sLink.Length = 6; sLink.MaximumLength = 7; sLink.Buffer = linkBuf;
    sDev.Length = (USHORT)devLen; sDev.MaximumLength = (USHORT)(devLen + 1);
    sDev.Buffer = (char*)dev;
    IoCreateSymbolicLink(&sLink, &sDev);
    s_mounted = 1;
}

static void UnmountDisc(void) {
    char namebuf[8];
    XBOX_STRING sName;
    if (!s_mounted) return;
    namebuf[0] = '\\'; namebuf[1] = '?'; namebuf[2] = '?'; namebuf[3] = '\\';
    namebuf[4] = DISC_LETTER; namebuf[5] = ':'; namebuf[6] = 0;
    sName.Length = 6; sName.MaximumLength = 7; sName.Buffer = namebuf;
    IoDeleteSymbolicLink(&sName);
    s_mounted = 0;
}

/* after mounting, identify the disc: Q:\default.xbe -> xbox game + title */
static void Identify(void) {
    DWORD attr;
    char  nm[64]; unsigned tid = 0;

    s_disc.isXboxGame = 0;
    s_disc.title[0] = 0;
    s_disc.xbePath[0] = 0;

    attr = GetFileAttributesA("Q:\\default.xbe");
    if (attr != 0xFFFFFFFF && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        s_disc.isXboxGame = 1;
        DcCopy(s_disc.xbePath, sizeof(s_disc.xbePath), "Q:\\default.xbe");
        if (Xbe_ReadTitle("Q:\\default.xbe", nm, sizeof(nm), &tid) && nm[0])
            DcCopy(s_disc.title, sizeof(s_disc.title), nm);
        else
            DcCopy(s_disc.title, sizeof(s_disc.title), "Game Disc");
    }
}

/* ---- public ----------------------------------------------------------- */

void Disc_Init(void) {
    s_disc.present = 0;
    s_disc.isXboxGame = 0;
    s_disc.title[0] = 0;
    s_disc.xbePath[0] = 0;
    s_mounted = 0;
    s_haveLast = 0;
    s_lastTray = 0xFFFFFFFF;
}

void Disc_Poll(void) {
    ULONG tray = 0, eject = 0;
    NTSTATUS st;

    st = HalReadSMCTrayState(&tray, &eject);
    if (st != 0) return;                  /* SMC read failed; leave state as-is */

    /* only act on a change in tray state */
    if (s_haveLast && tray == s_lastTray) return;
    s_lastTray = tray; s_haveLast = 1;

    if (tray == SMC_TRAY_STATE_CLOSED) {
        /* tray shut: assume media present, mount + identify */
        MountDisc();
        s_disc.present = 1;
        Identify();
    }
    else if (tray == SMC_TRAY_STATE_OPEN ||
        tray == SMC_TRAY_STATE_NO_MEDIA ||
        tray == SMC_TRAY_STATE_OPENING ||
        tray == SMC_TRAY_STATE_UNLOADING) {
        /* no usable media: clear everything */
        UnmountDisc();
        s_disc.present = 0;
        s_disc.isXboxGame = 0;
        s_disc.title[0] = 0;
        s_disc.xbePath[0] = 0;
    }
    /* transient states (CLOSING/MEDIA_DETECT/RESET): wait for a stable state */
}

const DiscState* Disc_Get(void) { return &s_disc; }

int Disc_Launch(void) {
    if (!s_disc.present || !s_disc.isXboxGame) return 0;
    Mount_LaunchXbe(s_disc.xbePath);   /* no return on success */
    return 0;                          /* only reached on failure */
}