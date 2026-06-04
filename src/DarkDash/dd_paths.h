#pragma once
/*---------------------------------------------------------------------------
    dd_paths.h -- user-defined extra scan paths for the launcher categories.

    The built-in scan roots (E:/F:/G:\Apps, \Games, etc.) cover the common
    setups, but some consoles have extra drive bays or unusual layouts. This
    lets the user add a few extra folders per category (Applications, Games,
    Homebrew, Emulators); the launcher scans them in addition to the built-ins.

    Stored in its own file (D:\data\paths.dat) so the main settings format is
    untouched. Keyed by the category's cacheId ("apps"/"games"/"homebrew"/"emu").
---------------------------------------------------------------------------*/
#ifndef DD_PATHS_H
#define DD_PATHS_H

#ifdef __cplusplus
extern "C" {
#endif

#define DDPATHS_PER_CAT   4     /* max custom paths per category */
#define DDPATHS_PATH_MAX  120   /* max chars per path            */

    void Paths_Load(void);          /* load from disk (call once at boot) */
    int  Paths_Save(void);          /* persist to disk; 1 ok, 0 fail      */

    /* How many custom paths are set for a category (by cacheId). */
    int  Paths_Count(const char* cacheId);

    /* The i-th custom path for a category, or "" if out of range. */
    const char* Paths_Get(const char* cacheId, int i);

    /* Add a path to a category. Returns 1 on success, 0 if full / invalid /
       duplicate. Does NOT auto-save -- caller calls Paths_Save(). */
    int  Paths_Add(const char* cacheId, const char* path);

    /* Remove the i-th path from a category (shifts the rest down). */
    void Paths_Remove(const char* cacheId, int i);

#ifdef __cplusplus
}
#endif
#endif /* DD_PATHS_H */