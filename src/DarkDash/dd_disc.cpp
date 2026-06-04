/*---------------------------------------------------------------------------
    dd_disc.cpp -- optical disc monitor (see dd_disc.h).

    SMC tray states (from xboxinternals.h):
        SMC_TRAY_STATE_CLOSED      0x00  (tray shut; media seated when present)
        SMC_TRAY_STATE_OPEN        0x10
        SMC_TRAY_STATE_NO_MEDIA    0x40
        ...transient: OPENING/CLOSING/UNLOADING/MEDIA_DETECT/RESET

    We treat a transition INTO "closed" as "media present" and mount once; a
    transition into OPEN / NO_MEDIA clears the mount. Cdrom0 is remapped to S:
    (a staging letter) so the dashboard's own D: assets stay valid.

    Build: MSVC2003/C89 style; file-scope statics; no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"
#include "dd_disc.h"
#include "dd_xbe.h"
#include "dd_mount.h"

#define DISC_DEVICE  "\\Device\\Cdrom0"
#define DISC_LETTER  'S'

static DiscState s_disc;
static int       s_mounted = 0;     /* S: currently symlinked? */
static ULONG     s_lastTray = 0xFFFFFFFF;
static int       s_haveLast = 0;
static DWORD     s_nextProbe = 0;     /* throttle empty-tray re-probes */

typedef STRING XBOX_STRING;

/* ---- helpers ---------------------------------------------------------- */

static int DcLen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void DcCopy(char* d, int cap, const char* s) {
    int i = 0; if (cap <= 0) return;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0;
}

/* remap Cdrom0 -> S: (dismount first so a fresh disc is re-read) */
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

    /* drop any prior S: link, then (re)create it pointing at Cdrom0 */
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

/* after mounting, identify the disc: <letter>:\default.xbe -> xbox game + title */
static void Identify(void) {
    DWORD attr;
    char  nm[64]; unsigned tid = 0;
    char  xbe[20];

    s_disc.isXboxGame = 0;
    s_disc.title[0] = 0;
    s_disc.xbePath[0] = 0;

    /* "<letter>:\default.xbe" built from DISC_LETTER so it tracks the mount */
    xbe[0] = DISC_LETTER; xbe[1] = ':'; xbe[2] = '\\';
    DcCopy(xbe + 3, (int)sizeof(xbe) - 3, "default.xbe");

    attr = GetFileAttributesA(xbe);
    if (attr != 0xFFFFFFFF && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        s_disc.isXboxGame = 1;
        DcCopy(s_disc.xbePath, sizeof(s_disc.xbePath), xbe);
        if (Xbe_ReadTitle(xbe, nm, sizeof(nm), &tid) && nm[0])
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
    s_nextProbe = 0;
}

/* Is there actually readable media on the mounted drive? A freshly inserted
   disc reports "tray closed" before the kernel can read it, so we confirm by
   probing the device: list the volume root. Works for Xbox games, DVD video,
   and data discs (anything with a readable filesystem). */
static int DiscMediaReadable(void) {
    char            pat[8];
    WIN32_FIND_DATA fd;
    HANDLE          h;

    pat[0] = DISC_LETTER; pat[1] = ':'; pat[2] = '\\'; pat[3] = '*'; pat[4] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;   /* no media / not ready yet */
    FindClose(h);
    return 1;
}

void Disc_Poll(void) {
    ULONG tray = 0, eject = 0;
    NTSTATUS st;
    int closedish;

    st = HalReadSMCTrayState(&tray, &eject);
    if (st != 0) return;                  /* SMC read failed; leave state as-is */

    /* "closed-ish": the tray is shut or settling toward shut. We treat all of
       these as "a disc may be readable" and let the actual mount+probe decide,
       rather than trusting a single exact state value (real SMCs bounce through
       MEDIA_DETECT/CLOSING before resting, and some report non-0x00 when a disc
       is seated). */
    closedish = (tray == SMC_TRAY_STATE_CLOSED ||
        tray == SMC_TRAY_STATE_CLOSING ||
        tray == SMC_TRAY_STATE_MEDIA_DETECT);

    if (closedish) {
        /* If we don't already have a game mounted, (re)mount and probe. We
           re-probe (throttled) until we find media or the tray opens -- a
           freshly inserted disc needs a moment before the kernel can read it,
           so one shot isn't enough. The throttle avoids thrashing the kernel
           with a mount/dismount every frame when the tray is closed+empty. */
        if (!s_disc.present || !s_disc.isXboxGame) {
            DWORD now = GetTickCount();
            if (now >= s_nextProbe) {
                s_nextProbe = now + 1000;     /* re-probe at most ~1/sec */
                MountDisc();
                Identify();
                s_disc.present = DiscMediaReadable();
                if (!s_disc.present) UnmountDisc();   /* nothing there yet */
            }
        }
        s_lastTray = tray; s_haveLast = 1;
    }
    else if (tray == SMC_TRAY_STATE_OPEN ||
        tray == SMC_TRAY_STATE_NO_MEDIA ||
        tray == SMC_TRAY_STATE_OPENING ||
        tray == SMC_TRAY_STATE_UNLOADING) {
        /* no usable media: clear everything (only if we had something) */
        if (s_disc.present || s_mounted) {
            UnmountDisc();
            s_disc.present = 0;
            s_disc.isXboxGame = 0;
            s_disc.title[0] = 0;
            s_disc.xbePath[0] = 0;
        }
        s_lastTray = tray; s_haveLast = 1;
    }
    /* RESET / ACTIVITY etc: leave state as-is */
}

const DiscState* Disc_Get(void) { return &s_disc; }

int Disc_Launch(void) {
    if (!s_disc.present || !s_disc.isXboxGame) return 0;
    Mount_LaunchXbe(s_disc.xbePath);   /* no return on success */
    return 0;                          /* only reached on failure */
}