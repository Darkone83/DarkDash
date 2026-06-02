/*---------------------------------------------------------------------------
    dd_launcher.h -- shared "app browser" engine for APPLICATIONS / GAMES /
    HOMEBREW. One implementation; each menu is a thin wrapper that hands this
    a LauncherConfig (its title, empty message, and scan roots).

    A1 (this step): directory scan + scrollable list + cursor nav.
    Later: A2 real names from the XBE cert, A3 rotating title image on the
    pedestal, A4 launch via XLaunchNewImage, A5 caching + async scan.
---------------------------------------------------------------------------*/
#ifndef DD_LAUNCHER_H
#define DD_LAUNCHER_H

#include <xtl.h>

typedef struct {
    const char* title;      /* header label, e.g. "APPLICATIONS"  */
    const char* emptyMsg;   /* shown when nothing is found         */
    const char* const* roots;      /* directories to scan for app folders */
    int                rootCount;  /* number of entries in roots[]        */
    const char* cacheId;    /* cache filename stem in D:\data\, e.g. "apps" */
} LauncherConfig;

void Launcher_Enter(const LauncherConfig* cfg);   /* scan + reset cursor  */
int  Launcher_Update(WORD pressed, WORD held);     /* nav; returns 1 on B  */
void Launcher_Render(void);                        /* draw the screen      */

#endif /* DD_LAUNCHER_H */