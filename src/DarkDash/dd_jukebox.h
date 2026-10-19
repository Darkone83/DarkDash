#pragma once
#ifndef DARKDASH_JUKEBOX_H
#define DARKDASH_JUKEBOX_H
/*---------------------------------------------------------------------------
    dd_jukebox -- lightweight DarkDash music browser + playlists.

    Reuses the existing dd_audio streaming engine, ID3/artwork parser, OSK and
    folder picker. The music root is user-selected and persisted; browsing is
    lazy (one directory at a time) so there is no boot-time media database.
---------------------------------------------------------------------------*/
#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    void Jukebox_Init(void);
    void Jukebox_Shutdown(void);
    void Jukebox_Enter(void);
    int  Jukebox_IsOpen(void);

    /* Pump input/audio ownership. Returns 1 on the frame the Jukebox finishes
       closing and gives background-music ownership back to Settings. */
    int  Jukebox_Update(WORD pressed, WORD held);

    /* Draw over the already-rendered main-menu scene. This deliberately keeps
       DarkDash's ambient backdrop/orb/arcs alive underneath the Jukebox mode. */
    void Jukebox_Render(void);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_JUKEBOX_H */