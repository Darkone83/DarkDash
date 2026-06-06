/*---------------------------------------------------------------------------
    dd_rgb.h -- XBOX-RGB controller over UDP (DarkoneCustoms).

    XBOX-RGB is JSON on UDP 7777 and self-advertises (discovery in dd_udp is
    passive -- we just listen for its "XBOX RGB" broadcast). This module sends
    the control ops the firmware accepts: preview (live, no save), save (persist
    to NVS), reset (factory), and a brightness/mode/color quick-set wrapped in
    the firmware's "cfg" object.

    cfg schema (from RGBCtrl): brightness(0-255), mode(0..RGB_MODE_COUNT-1),
    speed(0-255), intensity(0-255), colorA..D (packed 0xRRGGBB). Menus gate on
    Udp_Present(UDP_DEV_RGB).
---------------------------------------------------------------------------*/
#ifndef DD_RGB_H
#define DD_RGB_H

#ifdef __cplusplus
extern "C" {
#endif

    /* mode list mirrors RGBCtrl's enum order */
    enum { RGB_MODE_COUNT = 16 };
    const char* Rgb_ModeName(int mode);

    void Rgb_Init(void);
    int  Rgb_Present(void);             /* convenience: Udp_Present(UDP_DEV_RGB)    */

    /* control ops (no-op + return 0 if RGB isn't present). 'save' nonzero persists
       to NVS; zero is a live preview only. */
    int  Rgb_SetMode(int mode, int save);
    int  Rgb_SetBrightness(int v0_255, int save);
    int  Rgb_SetSpeed(int v0_255, int save);
    int  Rgb_SetIntensity(int v0_255, int save);
    int  Rgb_SetPaletteCount(int n1_4, int save);
    /* slot 0..3 = colorA..D, packed 0xRRGGBB */
    int  Rgb_SetColor(int slot, unsigned long rgb, int save);
    int  Rgb_SetColorA(unsigned long rgb, int save);   /* kept: colorA convenience */
    int  Rgb_Reset(void);

    /* how many of colorA..D a given mode actually uses:
         0  = generated (Rainbow/Plasma/Fire/Custom) -- no editable colors
         1,2 = fixed count (e.g. Solid=1, ClockSpin=2)
        -1  = palette mode -- uses paletteCount (1..4) of A..D */
    int  Rgb_ModeColorCount(int mode);

#ifdef __cplusplus
}
#endif
#endif /* DD_RGB_H */