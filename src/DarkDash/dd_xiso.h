#ifndef DD_XISO_H
#define DD_XISO_H
/*---------------------------------------------------------------------------
    dd_xiso.h -- XDVDFS (Xbox ISO) reader.

    Validates an XISO and pulls files out of its root directory -- enough for
    the ISO->HDD installer to extract default.xbe / game.xbe. The game data
    itself is NOT extracted: the installer keeps the .iso whole and builds a
    cert-patched attach.xbe stub that Cerbios mounts and boots.

    Single-image XISOs only (the common case). Split _1/_2 images are not yet
    handled; a file whose data starts past the image's end reports failure.
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

    int Xiso_IsValid(const char* isoPath);     /* 1 if the XDVDFS signature is present */

    /* Locate a file in the ROOT directory (case-insensitive). On success returns 1
       and fills *sectorOut (2048-byte LBA) and *sizeOut (bytes). */
    int Xiso_FindRoot(const char* isoPath, const char* name,
        unsigned* sectorOut, unsigned* sizeOut);

    /* Extract a root-directory file to destPath (buffered). Returns 1 on success. */
    int Xiso_ExtractRoot(const char* isoPath, const char* name, const char* destPath);

#ifdef __cplusplus
}
#endif
#endif /* DD_XISO_H */