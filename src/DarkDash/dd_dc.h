/*---------------------------------------------------------------------------
    dd_dc.h -- shared DarkoneCustoms config blob (D:\data\dc.dat).

    One versioned file holds settings for the UDP accessory family (Type-D now;
    XBOX-RGB, OXFP later). SAFE UPGRADE is the whole point: when an older
    version is found we migrate it -- carry the existing fields forward and
    default any new ones -- rather than wiping it. That way adding a field in a
    later version never destroys a user's existing accessory settings.

    To add a device's settings later: bump DC_VER, add fields to DcBlob, and
    extend DcMigrate() to fill the new fields with defaults when loading an
    older version.
---------------------------------------------------------------------------*/
#ifndef DD_DC_H
#define DD_DC_H

#ifdef __cplusplus
extern "C" {
#endif

    /* Loads dc.dat (migrating older versions), or initializes defaults if absent.
       Safe to call repeatedly; only the first call touches disk. */
    void Dc_Load(void);

    /* Persist the current blob to disk (always written at the current version). */
    void Dc_Save(void);

    /* ---- accessors (each device uses its own) ------------------------------ */
    int  Dc_TypeDEnabled(void);
    void Dc_SetTypeDEnabled(int on);

    /* Type-D "Now Playing" cover art pushed to the device on game launch. */
    int  Dc_TypeDArtEnabled(void);          /* XL (id 5)        */
    void Dc_SetTypeDArtEnabled(int on);
    int  Dc_TypeDCtrlArtEnabled(void);      /* regular Type-D (1-4) */
    void Dc_SetTypeDCtrlArtEnabled(int on);

    /* RGB last-used settings (persisted so they survive reboot) */
    int  Dc_RgbMode(void);       void Dc_SetRgbMode(int m);
    int  Dc_RgbBright(void);     void Dc_SetRgbBright(int b);
    int  Dc_RgbSpeed(void);      void Dc_SetRgbSpeed(int s);

    /* OXFP last-used settings */
    int  Dc_OxfpMode(void);      void Dc_SetOxfpMode(int m);
    int  Dc_OxfpBright(void);    void Dc_SetOxfpBright(int b);

    /* commit several at once (e.g. on a Save action) without N disk writes */
    void Dc_SaveRgb(int mode, int bright, int speed);
    void Dc_SaveOxfp(int mode, int bright);

    /* v3 extended saves: full color/anim state (palette indices for colors) */
    void Dc_SaveRgb2(int mode, int bright, int speed, int intensity, int palCount,
        int colA, int colB, int colC, int colD);
    void Dc_SaveOxfp2(int mode, int bright, int anim, int animSpeed,
        int green, int red, int orange, int animA, int animB);

    /* v3 getters for the extended fields (colors are palette indices) */
    int  Dc_RgbIntensity(void); int Dc_RgbPalCount(void);
    int  Dc_RgbColor(int slot);                 /* slot 0..3 -> palette index        */
    int  Dc_OxfpAnim(void);     int Dc_OxfpAnimSpeed(void);
    int  Dc_OxfpStatus(int which);              /* 0..2 green/red/orange -> pal index */
    int  Dc_OxfpAnimColor(int ab);              /* 0/1 -> palette index               */

#ifdef __cplusplus
}
#endif
#endif /* DD_DC_H */