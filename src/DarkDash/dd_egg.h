/*---------------------------------------------------------------------------
    dd_egg.h -- main-menu easter egg: a spinning Darkone83 logo quad.

    Toggled by Black+RT on the main menu. When active, the orb is replaced by
    darkone83.png (from D:\data) rotating on its Y axis as a textured quad.
    The image loads lazily on first activation; state is not persisted.
---------------------------------------------------------------------------*/
#ifndef DD_EGG_H
#define DD_EGG_H

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    void Egg_Toggle(void);     /* flip active on/off (loads the image on first on) */
    int  Egg_Active(void);     /* 1 if the spinning logo should be shown          */
    void Egg_Draw(int cx, int cy, int w, int h);  /* draw the spinning quad over a screen region */
    void Egg_Shutdown(void);   /* release the texture */

#ifdef __cplusplus
}
#endif
#endif /* DD_EGG_H */