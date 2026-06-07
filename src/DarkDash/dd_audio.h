#ifndef DARKDASH_AUDIO_H
#define DARKDASH_AUDIO_H
/*---------------------------------------------------------------------------
    dd_audio -- DirectSound SFX + music for DarkDash, decoded with minimp3.

    Layout (runtime, 'D:' = launched XBE home):
        D:\Media\audio\snd\*.mp3      one-shot SFX
        D:\Media\audio\music\bg.mp3   looping background music

    SFX and music are decoded fully to PCM at init and held in DS buffers.
    (bg.mp3 is decoded fully too; if it ever becomes a long track, switch it
    to a DirectSoundStream -- the header supports DirectSoundCreateStream.)
---------------------------------------------------------------------------*/
#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* SFX ids -> files in audio\snd (see k_sfx_files in dd_audio.cpp) */
    enum {
        SFX_NAV_UP = 0,   /* button-up.mp3    */
        SFX_NAV_DOWN,     /* button-down.mp3  */
        SFX_SELECT,       /* button-a.mp3     */
        SFX_ALT,          /* button-x.mp3     */
        SFX_BACK,         /* button-other.mp3 */
        SFX_COUNT
    };

    int  Audio_Init(void);            /* DirectSound + decode SFX bank + bg music */
    void Audio_Shutdown(void);

    /* Service the Xbox DirectSound mixer. MUST be called once per frame, or the
       play cursor stalls and nothing is audible (DirectSoundDoWork). */
    void Audio_Update(void);

    void Audio_PlaySfx(int id);       /* one-shot, restarts from the beginning */
    void Audio_StartMusic(int loop);  /* play bg.mp3 (loop!=0 to loop) */
    void Audio_StopMusic(void);

    /* 1 when a non-looping track has reached its natural end (and its buffered
       tail has drained). 0 while looping or stopped. Drives Shuffle advancing. */
    int  Audio_MusicFinished(void);

    /* Background-music volume, 0..100 percent. Set takes effect live if music is
       already playing, and is re-applied on the next StartMusic. */
    void Audio_SetMusicVolume(int pct);
    int  Audio_GetMusicVolume(void);

    /* Set the bg track by full path (NULL/empty -> built-in bg.mp3). Applies on
       the next StartMusic. */
    void Audio_SetMusicPath(const char* fullPath);

    /* Diagnostics: see exactly what succeeded at init. */
    int  Audio_DbgReady(void);        /* 1 if the DirectSound device was created  */
    int  Audio_DbgSfxLoaded(void);    /* count of SFX buffers loaded (0..SFX_COUNT)*/
    int  Audio_DbgMusicLoaded(void);  /* 1 if the bg music buffer was created      */
    int  Audio_DbgFilesRead(void);    /* files lodepng read off disk (0..6)        */
    int  Audio_DbgFilesDecoded(void); /* files minimp3 decoded to PCM (0..6)       */

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_AUDIO_H */