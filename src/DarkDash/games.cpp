/*---------------------------------------------------------------------------
    Games.cpp -- GAMES scan roots: \Games on E:/F:/G:, each game a subfolder
    holding a default.xbe.
---------------------------------------------------------------------------*/
#include "Games.h"

static const char* const k_gameRoots[] = {
    "E:\\Games", "F:\\Games", "G:\\Games"
};

static const LauncherConfig k_gameCfg = {
    "GAMES",
    "No games found",
    k_gameRoots,
    3,
    "games"
};

const LauncherConfig* Games_Config(void) { return &k_gameCfg; }