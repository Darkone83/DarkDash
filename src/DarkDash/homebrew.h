/*---------------------------------------------------------------------------
    Homebrew.h -- HOMEBREW menu: scan roots for the shared launcher.
    Sibling of Applications/Games/Emulators. Scans \Homebrew on each drive.
---------------------------------------------------------------------------*/
#ifndef HOMEBREW_H
#define HOMEBREW_H

#include "dd_launcher.h"

const LauncherConfig* Homebrew_Config(void);

#endif /* HOMEBREW_H */