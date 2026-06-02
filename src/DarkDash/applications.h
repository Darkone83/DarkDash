/*---------------------------------------------------------------------------
    Applications.h -- APPLICATIONS menu: scan roots for the shared launcher.

    Thin sibling of Games/Homebrew/Emulators. It owns nothing but its scan
    roots and labels; it hands a LauncherConfig to the shared dd_launcher
    engine, which does all the work. main.cpp drives the launcher directly
    using the config returned here.
---------------------------------------------------------------------------*/
#ifndef APPLICATIONS_H
#define APPLICATIONS_H

#include "dd_launcher.h"

const LauncherConfig* App_Config(void);

#endif /* APPLICATIONS_H */