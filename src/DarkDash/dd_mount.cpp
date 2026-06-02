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

/* STRING (kernel counted-string) and the Io*SymbolicLink externs now come from
   xboxinternals.h. Local alias keeps the existing call sites readable. */
typedef STRING XBOX_STRING;

struct DriveMap { const char* letter; const char* device; };

static const DriveMap k_drives[] = {
    { "C", "\\Device\\Harddisk0\\Partition2" },
    { "E", "\\Device\\Harddisk0\\Partition1" },
    { "F", "\\Device\\Harddisk0\\Partition6" },
    { "G", "\\Device\\Harddisk0\\Partition7" },
    { "X", "\\Device\\Harddisk0\\Partition3" },
    { "Y", "\\Device\\Harddisk0\\Partition4" },
    { "Z", "\\Device\\Harddisk0\\Partition5" }
};

void Mount_HddPartitions(void) {
    char linkBuf[8];
    int  i, devLen;

    for (i = 0; i < 7; ++i) {
        const char* dev = k_drives[i].device;

        /* "\??\X:" -- 6 chars, plus NUL */
        linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
        linkBuf[4] = k_drives[i].letter[0];
        linkBuf[5] = ':';  linkBuf[6] = '\0';

        devLen = 0; while (dev[devLen]) devLen++;

        {
            XBOX_STRING sLink = { 6, 7, linkBuf };
            XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), (char*)dev };
            IoCreateSymbolicLink(&sLink, &sDev);   /* 0xC0000035 = already mounted; ignore */
        }
    }
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
    char        devPath[300];
    char        launchPath[64];
    char        linkBuf[8];

    if (!dosPath || !dosPath[0]) return 1;
    drive = (dosPath[0] >= 'a' && dosPath[0] <= 'z')
        ? (char)(dosPath[0] - 32) : dosPath[0];

    /* D: is the running title's own device -- launch directly, no remap. */
    if (drive == 'D') { XLaunchNewImage((char*)dosPath, NULL); return 1; }

    for (i = 0; i < 7; ++i)
        if (k_drives[i].letter[0] == drive) { device = k_drives[i].device; break; }
    if (!device) return 2;

    len = 0; while (dosPath[len]) len++;
    lastBs = -1;
    for (i = 0; i < len; ++i) if (dosPath[i] == '\\') lastBs = i;
    if (lastBs < 2) return 3;               /* need at least "X:\file" */

    /* devPath = partition device + the directory part after "X:" (indices 2..lastBs) */
    j = 0;
    while (device[j] && j < (int)sizeof(devPath) - 1) { devPath[j] = device[j]; j++; }
    for (k = 2; k < lastBs && j < (int)sizeof(devPath) - 1; ++k) devPath[j++] = dosPath[k];
    devPath[j] = '\0';

    /* remap "\??\D:" -> devPath */
    linkBuf[0] = '\\'; linkBuf[1] = '?'; linkBuf[2] = '?'; linkBuf[3] = '\\';
    linkBuf[4] = 'D';  linkBuf[5] = ':';  linkBuf[6] = '\0';
    {
        int devLen = 0; while (devPath[devLen]) devLen++;
        XBOX_STRING sLink = { 6, 7, linkBuf };
        XBOX_STRING sDev = { (USHORT)devLen, (USHORT)(devLen + 1), devPath };
        IoDeleteSymbolicLink(&sLink);
        if (IoCreateSymbolicLink(&sLink, &sDev) != 0) return 4;
    }

    /* build "D:\<filename>" from the part after the last backslash */
    k = 0;
    launchPath[k++] = 'D'; launchPath[k++] = ':'; launchPath[k++] = '\\';
    for (i = lastBs + 1; dosPath[i] && k < (int)sizeof(launchPath) - 1; ++i)
        launchPath[k++] = dosPath[i];
    launchPath[k] = '\0';

    XLaunchNewImage(launchPath, NULL);
    return 5;                               /* only reached if the launch failed */
}