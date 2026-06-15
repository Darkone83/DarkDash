#ifndef DD_ATTACH_H
#define DD_ATTACH_H
/*---------------------------------------------------------------------------
    dd_attach.h -- build a Cerbios attach.xbe for an installed XISO.

    Loads the shipped Cerbios attach template (D:\data\attach.xbe), stamps it
    with the game's certificate (title, ID, region) + version flag, embeds the
    game's title image so the launcher shows the right icon, and writes the
    result. Cerbios sees the stamped stub and mounts the co-located .iso.

    All offsets verified against the shipped attach.xbe and the Rocky5 installer.
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

    /* Build an attach stub for gameXbePath (the default.xbe extracted from the ISO),
       writing it to outAttachPath. Returns 1 on success, 0 on failure (e.g. the
       template is missing from D:\data). */
    int Attach_Build(const char* gameXbePath, const char* outAttachPath);

#ifdef __cplusplus
}
#endif
#endif /* DD_ATTACH_H */