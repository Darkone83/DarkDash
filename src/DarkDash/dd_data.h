/*---------------------------------------------------------------------------
    dd_data.h -- DarkDash persistent settings.

    One small versioned blob at D:\data\settings.dat. Load once at boot;
    mutate the struct in place via Data_Get(); call Data_Save() after a change.
    Writes are best-effort and guarded -- on a read-only volume (e.g. xemu)
    the save silently fails and the in-memory copy keeps working, so nothing
    crashes and the UI still reflects the user's choices for the session.

    Everything here is a dashboard preference. None of it touches the EEPROM.
---------------------------------------------------------------------------*/
#ifndef DD_DATA_H
#define DD_DATA_H

#ifdef __cplusplus
extern "C" {
#endif

#define DD_MUSIC_PATH_MAX 260
#define DD_FTP_CRED_MAX   32

    /* video fill mode (drives the UI scaler) */
    enum { DD_VIDEO_PILLARBOX = 0, DD_VIDEO_STRETCH = 1 };

    /* video resolution preference (applied at boot in Gfx_Init) */
    enum { DD_RES_AUTO = 0, DD_RES_480 = 1, DD_RES_720 = 2 };

    typedef struct DD_Settings {
        int  musicVolume;                    /* 0..100                          */
        int  musicCustom;                    /* 1 = use musicPath, 0 = built-in */
        char musicPath[DD_MUSIC_PATH_MAX];   /* custom bg track (picker)        */

        int  ftpEnabled;                     /* 1 = run FTP service at boot     */
        int  ftpPort;                        /* control port (default 21)       */
        char ftpUser[DD_FTP_CRED_MAX];       /* login user (default "xbox")     */
        char ftpPass[DD_FTP_CRED_MAX];       /* login pass (default "xbox")     */

        int  fanAuto;                        /* 1 = let the SMC manage the fan  */
        int  fanPercent;                     /* manual fan duty 0..100          */

        int  videoAspect;                    /* DD_VIDEO_PILLARBOX / DD_VIDEO_STRETCH */
        int  videoRes;                       /* DD_RES_AUTO / DD_RES_480 / DD_RES_720  */

        char fontName[64];                   /* .ddf basename in D:\fonts (empty = Default) */
        char themeName[64];                  /* folder in D:\themes (empty = default)       */

        /* room to grow without bumping the on-disk version every time */
        int  reserved[8];
    } DD_Settings;

    /* Load settings.dat into memory (defaults if missing/old/corrupt). Safe to
       call once at boot. Returns 1 if a valid file was read, 0 if defaults used. */
    int  Data_Load(void);

    /* Persist the current in-memory settings. Returns 1 on success, 0 if the
       write was refused (read-only volume) -- caller can ignore the result. */
    int  Data_Save(void);

    /* Mutable pointer to the live settings (never NULL after Data_Load). */
    DD_Settings* Data_Get(void);

#ifdef __cplusplus
}
#endif
#endif /* DD_DATA_H */