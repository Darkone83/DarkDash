/*---------------------------------------------------------------------------
    dd_screensaver.h -- idle screensaver: the DarkDash pedestal, drifting slowly
    across a dark field, fading random cover art in and out, with the light
    shaft cycling through an RGB rainbow.

    Driven from the main loop's idle timer: when the configured idle timeout
    elapses, call Saver_Enter() once; then Saver_Update()/Saver_Render() each
    frame until any input arrives, at which point call Saver_Exit().

    The title pool is scanned lazily on first Enter from the same drive roots
    the launcher uses (Applications/Games/Homebrew/Emulators), so it showcases
    the user's own library. Art uses the launcher's exact priority chain.
---------------------------------------------------------------------------*/
#ifndef DD_SCREENSAVER_H
#define DD_SCREENSAVER_H

#ifdef __cplusplus
extern "C" {
#endif

    void Saver_Enter(void);    /* begin the screensaver (scans the pool once)   */
    void Saver_Update(void);   /* advance the fade/drift/rainbow state machine   */
    void Saver_Render(void);   /* draw one frame (caller does Begin/EndFrame)    */
    void Saver_Exit(void);     /* leave the screensaver; frees the current art   */

#ifdef __cplusplus
}
#endif
#endif /* DD_SCREENSAVER_H */