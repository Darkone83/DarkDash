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

    /* OXFP base modes (firmware OXFP_Mode): Stock=0, Static=1, Animation=2. */
    enum { OXFP_MODE_COUNT = 3 };
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

    /* animation: mode, two colors, speed. 11 anim modes (firmware OXFP_AnimMode). */
    enum { OXFP_ANIM_COUNT = 11 };
    const char* Oxfp_AnimName(int anim);
    int  Oxfp_SetAnimMode(int anim);
    int  Oxfp_SetAnimColor(int ab, unsigned long rgb);   /* ab: 0=A, 1=B           */
    int  Oxfp_SetAnimSpeed(int v0_255);

    /* ---- live config read-back -------------------------------------------- */
    /* Ask the device to report its current config (reply lands in dd_udp's
       capture buffer; poll it with Udp_LastReply, then Oxfp_ParseConfig). */
    int  Oxfp_RequestConfig(void);

    /* Parsed snapshot of an OXFP "get" reply. Any field the reply didn't carry
       is left as -1 (ints) or has [0] == -1 (colors), so callers can apply only
       what was actually present. */
    typedef struct {
        int mode, brightness, animMode, animSpeed;
        int green[3], red[3], orange[3];
        int animA[3], animB[3];
    } OxfpDevCfg;

    /* Parse a captured datagram. Returns 1 if it looked like an OXFP config
       reply (at least one known field found), 0 otherwise. */
    int  Oxfp_ParseConfig(const char* json, int len, OxfpDevCfg* out);

#ifdef __cplusplus
}
#endif
#endif /* DD_OXFP_H */