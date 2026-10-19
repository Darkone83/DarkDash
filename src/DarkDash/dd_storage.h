#ifndef DARKDASH_STORAGE_H
#define DARKDASH_STORAGE_H
/*--------------------------------------------------------------------------
    dd_storage -- single source of truth for internal HDD partitions.

    HDD0 stays on C/E/F/G/X/Y/Z. CerBIOS publishes HDD1 kernel objects as
    \Device\Harddisk1\Partition#. Secondary data aliases: N=E (1),
    O=F (6), P=G (7), R=C (2). Nothing is formatted, and preexisting aliases
    are never replaced. Mounting requires an accessible directory-root check.
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

#define DD_STORAGE_HDD0 0
#define DD_STORAGE_HDD1 1

    typedef struct DD_StorageVolume {
        char letter;
        int disk;                 /* 0 = primary, 1 = secondary */
        int partition;
        const char* device;      /* Xbox kernel device path */
        int probed;              /* NtOpenFile succeeded with read-attributes */
        int mounted;             /* symbolic-link creation succeeded */
    } DD_StorageVolume;

    int DD_Storage_Count(void);
    const DD_StorageVolume* DD_Storage_At(int index);
    const DD_StorageVolume* DD_Storage_Letter(char letter);

    /* Called by Mount_HddPartitions; retains original primary link replacement. */
    void DD_Storage_MountPrimary(void);

    /* Only tries NtOpenFile(..., FILE_READ_ATTRIBUTES, ...); never creates links,
       formats, unlocks, or writes anything. Returns number of HDD1 partitions
       whose kernel device objects were actually openable. */
    int DD_Storage_ProbeSecondary(void);

    /* Called after primary mount and the read-only probe during boot.
       Adds N/O/P/R ONLY for traversable roots, without replacing any link.
       Failed or invalid mounts are rolled back. Returns usable HDD1 aliases. */
    int DD_Storage_MountSecondary(void);

    /* Re-probe and activate newly available HDD1 partitions. Safe to call on
       each browser entry/rescan; never replaces occupied aliases or writes data. */
    int DD_Storage_RefreshSecondary(void);

    /* A registered volume is ready if it was mounted by this process and its
       DOS root resolves to an accessible directory. Safe for UI/FTP scans. */
    int DD_Storage_Ready(const DD_StorageVolume* volume);

    /* Returns a display label such as "N: (HDD1-E)" or "E:". */
    void DD_Storage_Label(const DD_StorageVolume* volume, char* out, int cap);

    /* FTP secondary virtual entry, e.g. "HDD1-E"; NULL for HDD0. */
    const char* DD_Storage_FtpName(const DD_StorageVolume* volume);

    /* Launch mapping resolver. HDD0 behaves like the original hardcoded map;
       HDD1 returns NULL until it has a confirmed alias. */
    const char* DD_Storage_LaunchDevice(char drive);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_STORAGE_H */
