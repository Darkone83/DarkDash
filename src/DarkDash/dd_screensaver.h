/*---------------------------------------------------------------------------
    dd_screensaver.h -- idle screensaver: the DarkDash pedestal, drifting slowly
    across a dark field, fading random cover art in and out, with the light
    shaft cycling through an RGB rainbow.

    Driven from the main loop's idle timer: when the configured idle timeout
    elapses, call Saver_Enter() once; then Saver_Update()/Saver_Render() each
    frame until any input arrives, at which point call Saver_Exit().

    The title pool is rebuilt on Enter from the same built-in roots, custom
    paths, custom categories, and recursive depth rules the launcher uses, so
    FTP additions/path changes show up without a dashboard restart. Art uses
    the launcher's exact priority chain.
---------------------------------------------------------------------------*/
#ifndef DD_SCREENSAVER_H
#define DD_SCREENSAVER_H

#ifdef __cplusplus
extern "C" {
#endif

    void Saver_Enter(void);    /* begin the screensaver (refreshes title pool)  */
    void Saver_Update(void);   /* advance the fade/drift/rainbow state machine   */
    void Saver_Render(void);   /* draw one frame (caller does Begin/EndFrame)    */
    void Saver_Exit(void);     /* leave the screensaver; frees the current art   */

#ifdef __cplusplus
}
#endif
#endif /* DD_SCREENSAVER_H */