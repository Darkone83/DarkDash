/*---------------------------------------------------------------------------
    Homebrew.cpp -- HOMEBREW scan roots: \Homebrew on E:/F:/G:, each app a
    subfolder holding a default.xbe.
---------------------------------------------------------------------------*/
#include "Homebrew.h"

static const char* const k_hbRoots[] = {
    "E:\\Homebrew", "F:\\Homebrew", "G:\\Homebrew"
};

static const LauncherConfig k_hbCfg = {
    "HOMEBREW",
    "No homebrew found",
    k_hbRoots,
    3,
    "homebrew"
};

const LauncherConfig* Homebrew_Config(void) { return &k_hbCfg; }