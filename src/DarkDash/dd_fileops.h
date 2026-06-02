#pragma once
/*---------------------------------------------------------------------------
    dd_fileops.h -- shared file operations + Memory Unit management.

    The common glue under both the file manager and the FTP server. Mechanics
    only (no UI): the six operations DarkDash needs --
        copy, delete, move, create dir, remove dir, rename
    -- plus the full MU system (enumerate / mount / format / free space / close).

    Two copy interfaces:
      * Blocking  -- Fileops_Copy / Move / Delete etc. Run to completion; FTP
                     and simple callers use these.
      * Chunked   -- FileopsCopy handle that does one buffer per Pump() call,
                     so the file manager can show progress without stalling the
                     render loop. Mirrors XbDiag's one-chunk-per-tick design.

    Paths are DOS-style ("E:\\Apps\\foo.xbe"). All ops are recursive where it
    makes sense (copy/move/delete of a directory walk their trees).
---------------------------------------------------------------------------*/
#ifndef DD_FILEOPS_H
#define DD_FILEOPS_H

#include <xtl.h>

#define FILEOPS_PATH_MAX  256
#define FILEOPS_COPY_BUF  (64 * 1024)   /* 64KB chunk, matches XbDiag */

#ifdef __cplusplus
extern "C" {
#endif

    /* ---- single-shot operations (blocking) ---------------------------------- */

    /* Copy one file (not a directory). Creates/overwrites dst. 1 on success. */
    int Fileops_CopyFile(const char* src, const char* dst);

    /* Copy a file OR directory tree from src to dst (dst is the full target path,
       not a parent). Recursively creates dirs + copies files. 1 if all succeeded. */
    int Fileops_CopyTree(const char* src, const char* dst);

    /* Delete a file, or a directory and everything under it. 1 if all removed. */
    int Fileops_Delete(const char* path);

    /* Move a file or tree (copy then delete source; source kept if copy failed). */
    int Fileops_Move(const char* src, const char* dst);

    /* Create / remove a single directory. mkdir ignores "already exists". */
    int Fileops_MkDir(const char* path);
    int Fileops_RmDir(const char* path);   /* must be empty; use Delete for trees */

    /* Rename (or move within the same volume) via the path table. */
    int Fileops_Rename(const char* oldPath, const char* newPath);

    /* Helpers */
    int Fileops_Exists(const char* path);      /* 1 if file or dir exists        */
    int Fileops_IsDir(const char* path);       /* 1 if exists and is a directory */

    /* ---- chunked copy (one buffer per Pump, for responsive progress) -------- */

    typedef struct {
        HANDLE src, dst;
        DWORD  total;      /* source size in bytes (0 if unknown)   */
        DWORD  done;       /* bytes copied so far                   */
        int    active;     /* 1 while a file is in flight           */
        int    ok;         /* 0 if any read/write failed            */
        char   buf[FILEOPS_COPY_BUF];
    } FileopsCopy;

    int  FileopsCopy_Begin(FileopsCopy* c, const char* src, const char* dst);
    int  FileopsCopy_Pump(FileopsCopy* c);   /* 1 = more to do, 0 = finished     */
    void FileopsCopy_Abort(FileopsCopy* c);  /* close handles mid-copy           */

    /* ---- Memory Unit management --------------------------------------------- */

    /* Mount all HDD partitions + any inserted MUs (idempotent). 1 if any MU bound. */
    int  Mu_MountAll(void);

    /* Drive letter for an MU at port (0-3) / slot (0-1); '?' if out of range.
       Safe table avoids HDD (C,E,F,G,X,Y,Z) and DVD (D). */
    char Mu_Letter(int port, int slot);

    /* Presence (delegates to input.cpp's XGetDeviceChanges tracking). */
    int  Mu_Present(int port, int slot);

    /* Dismount + FATX-format + remount the MU; rebinds its drive letter. 1 ok. */
    int  Mu_Format(int port, int slot);

    /* Flush + release an MU volume after a write session. */
    void Mu_Release(int port, int slot);

    /* Free / total bytes on a mounted drive (letter like "A:\\"); 1 on success. */
    int  Mu_FreeBytes(const char* driveRoot, unsigned __int64* freeOut,
        unsigned __int64* totalOut);

#ifdef __cplusplus
}
#endif

#endif /* DD_FILEOPS_H */