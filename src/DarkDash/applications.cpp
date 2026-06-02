/*---------------------------------------------------------------------------
    Applications.cpp -- APPLICATIONS scan roots.

    Homebrew-ready systems keep apps on E:/F:/G: under \Apps or \Applications,
    each app being a subfolder that contains a default.xbe.
---------------------------------------------------------------------------*/
#include "Applications.h"

static const char* const k_appRoots[] = {
    "E:\\Apps",         "F:\\Apps",         "G:\\Apps",
    "E:\\Applications", "F:\\Applications", "G:\\Applications"
};

static const LauncherConfig k_appCfg = {
    "APPLICATIONS",
    "No applications found",
    k_appRoots,
    6,
    "apps"
};

const LauncherConfig* App_Config(void) { return &k_appCfg; }