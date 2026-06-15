#ifndef DD_CUSTOM_H
#define DD_CUSTOM_H
/*---------------------------------------------------------------------------
    dd_custom.h -- user-defined launcher categories.

    A custom category is just a runtime LauncherConfig (a display name + one
    scan folder + an auto-generated cacheId), persisted to D:\data\custom.dat.
    It feeds the existing launcher engine unchanged -- the launcher never knows
    a category is "custom". cacheIds are auto-issued as cust0/cust1/... from a
    monotonic counter so a removed category's id is never reused (no stale
    cache/paths bleed into a later one).

    If custom.dat is missing it's simply treated as empty; the first Add creates
    it, later Adds append -- the user never has to think about the file.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_launcher.h"   /* LauncherConfig */

#define CUSTOM_MAX        16    /* max custom categories            */
#define CUSTOM_NAME_MAX   32    /* display title                    */
#define CUSTOM_PATH_MAX   120   /* scan folder (matches dd_paths)   */
#define CUSTOM_ID_MAX     16    /* "cust" + decimal counter + NUL   */

#ifdef __cplusplus
extern "C" {
#endif

    void Custom_Load(void);                       /* load custom.dat (lazy/once)   */
    int  Custom_Save(void);                       /* persist; 1 ok                 */
    int  Custom_Count(void);
    const LauncherConfig* Custom_Get(int i);      /* NULL if out of range          */
    int  Custom_Add(const char* name, const char* path);  /* 1 on success          */
    int  Custom_Remove(int i);                    /* 1 on success                  */

#ifdef __cplusplus
}
#endif
#endif /* DD_CUSTOM_H */