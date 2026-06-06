/*---------------------------------------------------------------------------
    dd_oxfp.h -- OXFP (front-panel RGB) control over UDP (DarkoneCustoms).

    OXFP is request/response on UDP 32123. It does not self-advertise, so
    discovery polls it with a ping (handled in dd_udp). This module sends the
    control ops the firmware understands: mode switch, brightness, the three
    status colors (green/red/orange), animation mode/colors/speed, plus
    preview / save / reset / identify.

    Menus that drive these should only act when Udp_Present(UDP_DEV_OXFP) is
    true -- otherwise grey out, so we never fire control packets into the void.

    JSON ops/fields mirror the firmware exactly:
        {"op":"mode","mode":N}
        {"op":"set","brightness":N,"greenColor":[r,g,b],...}  (live, ~preview)
        {"op":"save"} {"op":"reset"} {"op":"identify","ms":N}
---------------------------------------------------------------------------*/
#ifndef DD_OXFP_H
#define DD_OXFP_H

#ifdef __cplusplus
extern "C" {
#endif

    /* OXFP base modes (firmware OXFP_Mode). Names for the UI; values are ordinal. */
    enum { OXFP_MODE_COUNT = 4 };       /* adjust if firmware adds modes           */
    const char* Oxfp_ModeName(int mode);

    void Oxfp_Init(void);

    int  Oxfp_Present(void);            /* convenience: Udp_Present(UDP_DEV_OXFP)   */

    /* control ops (no-op + return 0 if OXFP isn't currently present) */
    int  Oxfp_SetMode(int mode);                   /* quick mode switch + render   */
    int  Oxfp_SetBrightness(int v0_255);           /* live brightness               */
    int  Oxfp_Identify(int ms);                    /* blink to locate               */
    int  Oxfp_Save(void);                          /* persist current firmware cfg  */
    int  Oxfp_Reset(void);                         /* factory defaults              */

    /* status colors -- the panel's green / red / orange state colors.
       which: OXFP_COL_GREEN / RED / ORANGE. rgb packed 0xRRGGBB (sent as [r,g,b]). */
    enum { OXFP_COL_GREEN = 0, OXFP_COL_RED = 1, OXFP_COL_ORANGE = 2, OXFP_COL_COUNT = 3 };
    int  Oxfp_SetStatusColor(int which, unsigned long rgb);

    /* animation: mode, two colors, speed */
    enum { OXFP_ANIM_COUNT = 4 };                  /* approximate; validate on HW   */
    const char* Oxfp_AnimName(int anim);
    int  Oxfp_SetAnimMode(int anim);
    int  Oxfp_SetAnimColor(int ab, unsigned long rgb);   /* ab: 0=A, 1=B           */
    int  Oxfp_SetAnimSpeed(int v0_255);

#ifdef __cplusplus
}
#endif
#endif /* DD_OXFP_H */