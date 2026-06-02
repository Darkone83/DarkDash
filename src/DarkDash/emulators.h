/*---------------------------------------------------------------------------
    Emulators.h -- EMULATORS menu: scan roots for the shared launcher.
    Sibling of Applications/Games/Homebrew. Scans both \emu and \emulators on
    each drive (different setups use either name).
---------------------------------------------------------------------------*/
#ifndef EMULATORS_H
#define EMULATORS_H

#include "dd_launcher.h"

const LauncherConfig* Emulators_Config(void);

#endif /* EMULATORS_H */