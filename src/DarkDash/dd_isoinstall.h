#ifndef DD_ISOINSTALL_H
#define DD_ISOINSTALL_H
/*---------------------------------------------------------------------------
    dd_isoinstall.h -- orchestrates a native XISO -> HDD install.

    Ties together dd_xiso (read the ISO), dd_attach (build the Cerbios stub),
    and the filesystem work: derive a folder name, extract default.xbe, stamp
    it into an attach.xbe, move the .iso in beside it, and drop a fallback
    thumbnail. The game data is never extracted -- Cerbios mounts the .iso.

    Destination is one of the GAMES category's configured roots: any user
    custom path first, then the E/F/G \Games defaults (first accessible).
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

    enum {
        ISOINST_OK = 0,
        ISOINST_ERR_NOTXISO,   /* not a valid XDVDFS image            */
        ISOINST_ERR_NODEST,    /* no accessible games root            */
        ISOINST_ERR_MKDIR,     /* could not create the target folder  */
        ISOINST_ERR_NOXBE,     /* no default.xbe / game.xbe in the ISO */
        ISOINST_ERR_ATTACH,    /* attach.xbe build failed             */
        ISOINST_ERR_MOVE,      /* could not move the .iso into place  */
        ISOINST_ERR_ARG
    };

    /* 1 if root is mounted/writable; also ensures the folder exists. */
    int IsoInstall_RootAccessible(const char* root);

    /* Resolve the install root (custom "games" paths first, then E/F/G \Games);
       ensures the chosen \Games exists. Returns 1 + fills out on success. */
    int IsoInstall_DefaultRoot(char* out, int cap);

    /* Install isoPath into destRoot\<Name> (ISO)\. Returns an ISOINST_* code. */
    int IsoInstall_Run(const char* isoPath, const char* destRoot);

    /* Convenience: resolve the default root, then IsoInstall_Run. */
    int IsoInstall_RunDefault(const char* isoPath);

#ifdef __cplusplus
}
#endif
#endif /* DD_ISOINSTALL_H */