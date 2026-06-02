/*---------------------------------------------------------------------------
    dd_mount.h -- mount the Xbox HDD partitions to drive letters.

    The kernel only auto-mounts D: (the running title's directory). Anything
    that touches C/E/F/G/X/Y/Z must symlink the letter to its partition device
    first. Call Mount_HddPartitions() once at boot before scanning drives.
    Idempotent -- safe to call again (already-mounted just returns an error
    code we ignore).
---------------------------------------------------------------------------*/
#ifndef DD_MOUNT_H
#define DD_MOUNT_H

void Mount_HddPartitions(void);

/* Launch the XBE at a DOS path ("E:\Apps\Foo\default.xbe"). Remaps D: to the
   target partition then XLaunchNewImage()s it. Does not return on success;
   returns non-zero on failure. */
int Mount_LaunchXbe(const char* dosPath);

#endif /* DD_MOUNT_H */