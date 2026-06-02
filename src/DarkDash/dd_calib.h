/*---------------------------------------------------------------------------
    dd_calib.h -- screen calibration (overscan adjustment) for DarkDash.

    Ported in spirit from XbDiag's ScreenCalib. The user drags four corner
    brackets inward to the visible edges of their TV; the resulting insets are
    applied globally through UI_SetCalibration (squeezing the whole 640x480
    virtual canvas into the safe area) and saved into DD_Settings.

    Auto-runs on first boot (when DD_Settings.calibrated == 0). Re-runnable any
    time from Settings -> Video.

    Margins are in VIRTUAL pixels (0..CALIB_MAX per edge).
---------------------------------------------------------------------------*/
#ifndef DD_CALIB_H
#define DD_CALIB_H

#ifdef __cplusplus
extern "C" {
#endif

#define CALIB_MAX  64    /* generous: up to 64 virtual px inset per edge */

    /* apply saved calibration from DD_Settings to the UI layer (call at boot,
       after UI_Init). Safe when uncalibrated -> zero insets. */
    void Calib_Apply(void);

    /* 1 if calibration has never been saved (first boot) -> caller should run it */
    int  Calib_NeedsRun(void);

    /* run the interactive bracket overlay. Blocks (own present loop) until the
       user saves (A) or cancels (B). On save: writes DD_Settings + applies live.
       Pumps input + presents each frame. */
    void Calib_Run(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_CALIB_H */