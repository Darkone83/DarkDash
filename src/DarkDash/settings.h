/*---------------------------------------------------------------------------
    Settings.h -- SETTINGS screen.

    The category list (Network, FTP, Video, Audio, Fan, Accessories, Clock,
    Theme, Font, Update, About) is navigable, and every category has a live
    editor screen behind it. Same screen contract as the launcher (enter /
    update-returns-1-on-B / render) so main.cpp routes it the same way.
---------------------------------------------------------------------------*/
#ifndef SETTINGS_H
#define SETTINGS_H

#include <xtl.h>

void Settings_Enter(void);
int  Settings_Update(WORD pressed, WORD held);  /* returns 1 to exit (B) */
void Settings_Render(void);

/* Resolve the saved background-music choice (Built-in / custom file / None /
   Shuffle) and (re)start playback. The single entry point used by both boot
   (main.cpp) and the Audio picker, so every mode behaves the same everywhere. */
void Settings_StartMusic(int loop);

/* Per-frame pump: advances Shuffle to the next random track when the current one
   ends. No-op in other music modes. Call once per frame from the main loop. */
void Settings_MusicTick(void);

/* Now-Playing toast: returns 1 while it should be drawn, filling *name and a
   0..1 *alpha (held, then faded). Only active just after a Shuffle track change. */
int Settings_NowPlaying(const char** name, float* alpha);

/* Live shuffle Now-Playing for the LCD page: 1 while Shuffle is playing a real
   track, filling *name and *elapsedMs (ms since the track began). 0 otherwise.
   name / elapsedMs may be NULL. */
int Settings_ShuffleNowPlaying(const char** name, DWORD* elapsedMs);

#endif /* SETTINGS_H */