#pragma once
/*---------------------------------------------------------------------------
    dd_backdrop.h -- the shared ambient backdrop every screen draws.

    One soft green bloom behind all content, pulsing on its own clock so every
    screen breathes identically. Drawing this first on each screen is what
    keeps the launcher / settings / file manager visually consistent with the
    main menu. Call Backdrop_Init() once at boot and Backdrop_Draw() at the top
    of each screen's render.
---------------------------------------------------------------------------*/
#ifndef DD_BACKDROP_H
#define DD_BACKDROP_H

int  Backdrop_Init(void);     /* build the glow texture; 1 on success */
void Backdrop_Draw(void);     /* ambient green bloom, self-pulsing    */
void Backdrop_Shutdown(void);

#endif /* DD_BACKDROP_H */