/*---------------------------------------------------------------------------
    Emulators.cpp -- EMULATORS scan roots: both \emu and \emulators on
    E:/F:/G: (setups vary on the folder name), each emulator a subfolder
    holding a default.xbe.
---------------------------------------------------------------------------*/
#include "Emulators.h"

static const char* const k_emuRoots[] = {
    "E:\\emu",        "F:\\emu",        "G:\\emu",
    "E:\\emulators",  "F:\\emulators",  "G:\\emulators"
};

static const LauncherConfig k_emuCfg = {
    "EMULATORS",
    "No emulators found",
    k_emuRoots,
    6,
    "emulators"
};

const LauncherConfig* Emulators_Config(void) { return &k_emuCfg; }