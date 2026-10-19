#pragma once
/*---------------------------------------------------------------------------
    dd_backdrop.h -- the shared ambient backdrop every screen draws.

    A single themed atmospheric renderer used behind every functional screen:
    home, launcher, settings, file manager and save manager. Existing plasma,
    calibration and screensaver rendering remain independent. Background
    movement is restricted to painted artwork, never functional controls.
---------------------------------------------------------------------------*/
#ifndef DD_BACKDROP_H
#define DD_BACKDROP_H

int  Backdrop_Init(void);     /* build the glow texture; 1 on success */
/* All functional screens use the same themed atmospheric backdrop. Home
   optionally supplies menu focus; every submenu gets the same light layers,
   while functional controls and text never move with the background. */
void Backdrop_Draw(void);
void Backdrop_DrawHome(float focus);
void Backdrop_Shutdown(void);

#endif /* DD_BACKDROP_H */