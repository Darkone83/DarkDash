/*---------------------------------------------------------------------------
    dd_eeprom.h -- safe read/write of the console audio flags.

    Audio mode (Stereo/Mono/Surround) and the AC3/DTS toggles live in the
    EEPROM-backed user config. We change them through the kernel's per-setting
    API -- ExSaveNonVolatileSetting(XC_AUDIO_FLAGS, ...) -- NOT by editing the
    raw 256-byte EEPROM image. The single-setting path lets the kernel handle
    the region's checksum itself, so there's no decrypt / HMAC / re-encrypt step
    and no risk to the security section. (This is the same call XbDiag uses for
    region writes, confirmed working on hardware.)

    Reading uses XGetAudioFlags() -- the kernel's cached value, always safe.

    Note: many titles read the audio config once at boot via XGetAudioFlags(),
    so a changed setting may only fully take effect after a relaunch/reboot.
---------------------------------------------------------------------------*/
#ifndef DD_EEPROM_H
#define DD_EEPROM_H

#ifdef __cplusplus
extern "C" {
#endif

    /* basic speaker mode */
    enum {
        DD_AUDIO_STEREO = 0,
        DD_AUDIO_MONO = 1,
        DD_AUDIO_SURROUND = 2
    };

    /* Read the current console audio config. Outparams may be NULL.
       *mode  -> DD_AUDIO_* ; *ac3 / *dts -> 0/1. Always succeeds (cached). */
    void Eeprom_GetAudio(int* mode, int* ac3, int* dts);

    /* Persist a new audio config to the EEPROM-backed user config via the kernel.
       Returns 1 on success, 0 on failure. Does not reboot -- caller decides. */
    int  Eeprom_SetAudio(int mode, int ac3, int dts);

#ifdef __cplusplus
}
#endif
#endif /* DD_EEPROM_H */