/*---------------------------------------------------------------------------
    dd_fx.h -- ambient "character" overlays (Phase 3 polish).

      - CRT scanlines + a slow vertical roll band, drawn over everything.
      - An edge-glow flash, triggered on select SFX, that pulses the screen
        border in the theme glow color.
      - A short boot intro flourish (glitch-in + sweep) so the dash powers on
        with personality instead of just appearing.

    All overlays draw in virtual 640x480 space via dd_ui, after the screen
    content and before EndFrame.
---------------------------------------------------------------------------*/
#ifndef DD_FX_H
#define DD_FX_H

#ifdef __cplusplus
extern "C" {
#endif

    int  Fx_Init(void);          /* build the scanline texture; 1 on success */
    void Fx_Shutdown(void);

    /* CRT overlay: faint scanlines + a slow brightness roll. Call each frame,
       late (over content). Cheap; safe to call even if Init failed. */
    void Fx_DrawScanlines(void);

    /* trigger the edge-glow flash (call when a select/confirm SFX plays) */
    void Fx_FlashEdge(void);
    /* draw the current edge-glow flash (decays on its own); call each frame late */
    void Fx_DrawEdgeGlow(void);

    /* boot intro: call Fx_BootBegin() once at startup; Fx_BootActive() is true
       until the intro finishes; Fx_DrawBoot() renders the current frame of it. */
    void Fx_BootBegin(void);
    int  Fx_BootActive(void);
    void Fx_DrawBoot(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_FX_H */