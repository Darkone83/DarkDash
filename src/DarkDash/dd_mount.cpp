/*---------------------------------------------------------------------------
    dd_mount.cpp -- map HDD partitions to drive letters via IoCreateSymbolicLink.

    Faithful port of XbDiag's FileExplorerMU.cpp::MountHDDDrives(): the same
    partition map and the same counted-string (XBOX_STRING) setup the kernel
    expects. F:/G: are the extended partitions that only exist on larger
    drives; if a partition isn't present the symlink simply leads nowhere and
    FindFirstFile on that letter fails harmlessly, so the scan skips it.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"
#include "dd_mount.h"
#include "dd_storage.h"
#include "dd_path.h"

/* STRING (kernel counted-string) and the Io*SymbolicLink externs now come from
   xboxinternals.h. Local alias keeps the existing call sites readable. */
typedef STRING XBOX_STRING;

/* Primary bindings are now owned by dd_storage.cpp. This wrapper retains the
   existing public API used by main.cpp and the MU module. Secondary aliases
   are activated separately by dd_storage after primary-mount boot probing. */
void Mount_HddPartitions(void) {
    DD_Storage_MountPrimary();
}

/*---------------------------------------------------------------------------
    Mount_SelfToD -- re-point D: at this XBE's own directory.

    XeImageFileName holds the device path the running image was loaded from,
    e.g. "\Device\Harddisk0\Partition2\xboxdash.xbe". We strip the trailing
    filename to get the directory device path and symlink "\??\D:" to it, so
    every "D:\..." asset path resolves to our install folder no matter how we
    were launched (title vs dashboard-from-Cerbios).
---------------------------------------------------------------------------*/
int Mount_SelfToD(void) {
    char  devPath[300];
    char  linkBuf[8];
    const char* src;
    int   srcLen, lastBs, i, j;

    if (!XeImageFileName || !XeImageFileName->Buffer) return 0;
    src = XeImageFileName->Buffer;
    srcLen = (int)XeImageFileName->Length;          /* counted, may not be NUL-terminated */
    if (srcLen <= 0 || srcLen > (int)sizeof(devPath) - 1) return 0;

    /* find the last backslash -> end of the directory part */
    lastBs = -1;
    for (i = 0; i < srcLen; ++i) if (src[i] == '\\') lastBs = i;
    if (lastBs <= 0) return 0;                       /* no directory component */

    /* copy "\Device\...\PartitionN<...dirs...>" up to (not incl) last backslash */
    for (j = 0; j < lastBs && j < (int)sizeof(devPath) - 1; ++j) devPath[j] = src[j];
    devPath[j] = '\0';

    /* remap "\??\D:" -> devPath */
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = 'D';  linkBuf[5] = ':';  linkBuf[6] = '\0';
    {
        int devLen = 0; while (devPath[devLen]) devLen++;
        XBOX_STRING sLink = { 6, 7, linkBuf };
        XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), devPath };
        IoDeleteSymbolicLink(&sLink);               /* drop any existing D: mapping */
        if (IoCreateSymbolicLink(&sLink, &sDev) != 0) return 0;
    }
    return 1;
}

/*---------------------------------------------------------------------------
    Mount_LaunchXbe -- boot the XBE at a DOS path (e.g. "E:\Apps\Foo\default.xbe").

    XLaunchNewImage only launches reliably from D: on RXDK, so we remap D: to
    the device path of the partition holding the XBE, then launch
    "D:\<filename>" -- the same pattern XbDiag's file explorer uses. On success
    this never returns. Returns non-zero only on failure (unknown drive, bad
    path, remap failure, or a launch that bounced back).
---------------------------------------------------------------------------*/
int Mount_LaunchXbe(const char* dosPath) {
    char        drive;
    const char* device = NULL;
    int         i, len, lastBs, j, k;
    char        devPath[DD_PATH_MAX + 64];
    char        launchPath[DD_PATH_MAX];
    char        linkBuf[8];

    if (!dosPath || !dosPath[0]) return 1;
    len = DD_PathLen(dosPath);
    if (len <= 3 || len >= DD_PATH_MAX || !DD_PathIsDosAbsolute(dosPath)) return 3;

    drive = (dosPath[0] >= 'a' && dosPath[0] <= 'z')
        ? (char)(dosPath[0] - 32) : dosPath[0];

    /* D: is the running title's own device -- launch directly, no remap. */
    if (drive == 'D') { XLaunchNewImage((char*)dosPath, NULL); return 1; }

    /* The optical disc (S:) isn't an HDD partition, so it's not in k_drives;
       it lives at \Device\Cdrom0. Resolve it explicitly so a game disc can be
       launched (remap D: -> Cdrom0, then XLaunchNewImage D:\default.xbe). */
    if (drive == 'S') {
        device = "\\Device\\Cdrom0";
    }
    else {
        device = DD_Storage_LaunchDevice(drive);
    }
    if (!device) return 2;

    lastBs = -1;
    for (i = 0; i < len; ++i) if (dosPath[i] == '\\') lastBs = i;
    if (lastBs < 2 || lastBs >= len - 1) return 3; /* need at least "X:\file" */

    /* devPath = device prefix + directory part after "X:". Refuse a path that
       does not fit rather than mounting a silently truncated directory. */
    j = 0;
    while (device[j]) {
        if (j >= (int)sizeof(devPath) - 1) return 3;
        devPath[j] = device[j];
        j++;
    }
    for (k = 2; k < lastBs; ++k) {
        if (j >= (int)sizeof(devPath) - 1) return 3;
        devPath[j++] = dosPath[k];
    }
    devPath[j] = '\0';

    /* remap "\??\D:" -> devPath */
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = 'D';  linkBuf[5] = ':';  linkBuf[6] = '\0';
    {
        int devLen = 0; while (devPath[devLen]) devLen++;
        XBOX_STRING sLink = { 6, 7, linkBuf };
        XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), devPath };
        IoDeleteSymbolicLink(&sLink);
        if (IoCreateSymbolicLink(&sLink, &sDev) != 0) {
            /* D: was removed before the failed remap; put DarkDash back on its
               own asset directory before returning to the caller. */
            Mount_SelfToD();
            return 4;
        }
    }

    /* build "D:\<filename>" from the part after the last backslash */
    /* A FATX leaf should already be small, but keep this exact: never turn an
       overlong launch request into a different/truncated filename. */
    if ((len - (lastBs + 1)) + 3 >= (int)sizeof(launchPath)) {
        Mount_SelfToD();
        return 3;
    }
    k = 0;
    launchPath[k++] = 'D'; launchPath[k++] = ':'; launchPath[k++] = '\\';
    for (i = lastBs + 1; dosPath[i]; ++i)
        launchPath[k++] = dosPath[i];
    launchPath[k] = '\0';

    XLaunchNewImage(launchPath, NULL);

    /* A successful launch never returns. If it does, immediately restore the
       dashboard's asset mount before the caller resumes rendering. */
    Mount_SelfToD();
    return 5;
}