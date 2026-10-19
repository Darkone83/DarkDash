/*---------------------------------------------------------------------------
    dd_disc.cpp -- optical disc monitor (see dd_disc.h).

    Cdrom0 is exposed to DarkDash as S: so the dashboard's own D: asset mount
    remains untouched. The monitor is intentionally stateful:

      empty/open -> settling -> readable/stable

    A media cycle mounts S: once, then probes that mapping until the filesystem
    is readable. Once readable (Xbox game, DVD video, or data disc), the mount
    is left alone until the tray/eject state changes. This avoids repeated
    mount/unmount churn while still tolerating slow optical spin-up.

    Build: MSVC2003/C89 style; file-scope statics; no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"
#include "dd_disc.h"
#include "dd_xbe.h"
#include "dd_mount.h"

#define DISC_DEVICE        "\\Device\\Cdrom0"
#define DISC_LETTER        'S'
#define DISC_PROBE_MS      750

enum {
    DISC_IDLE = 0,
    DISC_WAITING,
    DISC_STABLE
};

static DiscState s_disc;
static int       s_mounted = 0;       /* S: currently symlinked */
static int       s_probeState = DISC_IDLE;
static ULONG     s_lastTray = 0xFFFFFFFF;
static ULONG     s_lastEject = 0;
static int       s_haveLast = 0;
static DWORD     s_nextProbe = 0;

typedef STRING XBOX_STRING;

/* ---- helpers ---------------------------------------------------------- */

static int DcLen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void DcCopy(char* d, int cap, const char* s) {
    int i = 0; if (cap <= 0) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; } d[i] = 0;
}

static int TrayClosedish(ULONG tray) {
    return (tray == SMC_TRAY_STATE_CLOSED ||
        tray == SMC_TRAY_STATE_CLOSING ||
        tray == SMC_TRAY_STATE_MEDIA_DETECT) ? 1 : 0;
}

static int TrayOpenish(ULONG tray) {
    return (tray == SMC_TRAY_STATE_OPEN ||
        tray == SMC_TRAY_STATE_NO_MEDIA ||
        tray == SMC_TRAY_STATE_OPENING ||
        tray == SMC_TRAY_STATE_UNLOADING) ? 1 : 0;
}

static void ClearDiscInfo(void) {
    s_disc.present = 0;
    s_disc.isXboxGame = 0;
    s_disc.title[0] = 0;
    s_disc.xbePath[0] = 0;
}

/* Delete S: regardless of our local flag. This also cleans up a stale mapping
   left by an earlier title/session before creating a fresh one. */
static void UnmountDisc(void) {
    char namebuf[8];
    XBOX_STRING sName;
    namebuf[0] = '\\'; namebuf[1] = '?'; namebuf[2] = '?'; namebuf[3] = '\\';
    namebuf[4] = DISC_LETTER; namebuf[5] = ':'; namebuf[6] = 0;
    sName.Length = 6; sName.MaximumLength = 7; sName.Buffer = namebuf;
    IoDeleteSymbolicLink(&sName);            /* absent is harmless */
    s_mounted = 0;
}

/* Map Cdrom0 -> S:. Returns 1 only if the symlink was actually created. */
static int MountDisc(void) {
    char linkBuf[8];
    char namebuf[8];
    const char* dev = DISC_DEVICE;
    int devLen = DcLen(dev);
    XBOX_STRING sLink, sDev, sName;

    namebuf[0] = '\\'; namebuf[1] = '?'; namebuf[2] = '?'; namebuf[3] = '\\';
    namebuf[4] = DISC_LETTER; namebuf[5] = ':'; namebuf[6] = 0;
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = DISC_LETTER; linkBuf[5] = ':'; linkBuf[6] = 0;

    sName.Length = 6; sName.MaximumLength = 7; sName.Buffer = namebuf;
    IoDeleteSymbolicLink(&sName);             /* stale S: -> clean slate */

    sLink.Length = 6; sLink.MaximumLength = 7; sLink.Buffer = linkBuf;
    sDev.Length = (USHORT)devLen; sDev.MaximumLength = (USHORT)(devLen + 1);
    sDev.Buffer = (char*)dev;
    if (IoCreateSymbolicLink(&sLink, &sDev) != 0) {
        s_mounted = 0;
        return 0;
    }
    s_mounted = 1;
    return 1;
}

/* Is there actually readable media on S:? A freshly closed tray often reports
   closed before the filesystem is ready, so readiness is based on a real root
   enumeration rather than the SMC state alone. */
static int DiscMediaReadable(void) {
    char            pat[8];
    WIN32_FIND_DATA fd;
    HANDLE          h;

    if (!s_mounted) return 0;
    pat[0] = DISC_LETTER; pat[1] = ':'; pat[2] = '\\'; pat[3] = '*'; pat[4] = 0;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
}

/* Identify an already-readable disc. Non-Xbox media remains present+stable;
   it simply has isXboxGame=0 and can still be browsed in FileMan. */
static void Identify(void) {
    DWORD attr;
    char  nm[64]; unsigned tid = 0;
    char  xbe[20];

    s_disc.isXboxGame = 0;
    s_disc.title[0] = 0;
    s_disc.xbePath[0] = 0;

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

static void BeginMediaProbe(DWORD now) {
    UnmountDisc();
    ClearDiscInfo();
    s_probeState = DISC_WAITING;
    s_nextProbe = now;                       /* first attempt immediately */
}

/* ---- public ----------------------------------------------------------- */

void Disc_Init(void) {
    /* S: is DarkDash-owned staging space; clear any stale link first. */
    UnmountDisc();
    ClearDiscInfo();
    s_probeState = DISC_IDLE;
    s_haveLast = 0;
    s_lastTray = 0xFFFFFFFF;
    s_lastEject = 0;
    s_nextProbe = 0;
}

void Disc_Poll(void) {
    ULONG tray = 0, eject = 0;
    NTSTATUS st;
    DWORD now;
    int closedish;
    int openish;
    int mediaCycle = 0;

    st = HalReadSMCTrayState(&tray, &eject);
    if (st != 0) return;                      /* transient SMC failure: keep state */

    now = GetTickCount();
    closedish = TrayClosedish(tray);
    openish = TrayOpenish(tray);

    /* The eject counter catches a quick eject/reinsert that could happen
       entirely between DarkDash's ~500ms polls, where both observed tray
       states would otherwise be CLOSED and the old disc identity could stick. */
    if (s_haveLast && eject != s_lastEject) mediaCycle = 1;
    if (s_haveLast && closedish && !TrayClosedish(s_lastTray)) mediaCycle = 1;

    if (openish) {
        if (s_probeState != DISC_IDLE || s_mounted || s_disc.present) {
            UnmountDisc();
            ClearDiscInfo();
        }
        s_probeState = DISC_IDLE;
        s_nextProbe = 0;
    }
    else if (closedish) {
        if (!s_haveLast || mediaCycle || s_probeState == DISC_IDLE)
            BeginMediaProbe(now);

        if (s_probeState == DISC_WAITING && now >= s_nextProbe) {
            s_nextProbe = now + DISC_PROBE_MS;

            /* Link creation itself can fail while the drive/kernel is settling;
               retry it later, but once it succeeds leave it mounted. */
            if (!s_mounted && !MountDisc()) {
                /* remain DISC_WAITING */
            }
            else if (DiscMediaReadable()) {
                s_disc.present = 1;
                Identify();
                s_probeState = DISC_STABLE;  /* Xbox game OR other readable media */
            }
        }
    }
    /* RESET/ACTIVITY/other transient states: preserve the current state. */

    s_lastTray = tray;
    s_lastEject = eject;
    s_haveLast = 1;
}

const DiscState* Disc_Get(void) { return &s_disc; }

int Disc_Launch(void) {
    DWORD attr;
    if (!s_disc.present || !s_disc.isXboxGame || !s_disc.xbePath[0]) return 0;

    /* Last-moment sanity check so START never launches a stale identity if the
       media vanished before the next SMC poll. A failure falls back to the
       normal probe state rather than attempting a bad remap. */
    attr = GetFileAttributesA(s_disc.xbePath);
    if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ClearDiscInfo();
        s_probeState = DISC_WAITING;
        s_nextProbe = GetTickCount();
        return 0;
    }

    Mount_LaunchXbe(s_disc.xbePath);         /* no return on success */
    return 0;                                /* only reached on failure */
}