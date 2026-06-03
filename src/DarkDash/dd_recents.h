/*---------------------------------------------------------------------------
    dd_recents.h -- the last few launched titles.

    A tiny most-recently-used list persisted to D:\data\recents.dat. The
    launcher calls Recents_Add(label, xbePath) just before it boots a title;
    the main menu shows the list in a small overlay (names only) reachable with
    Y, and can relaunch any entry.

    Newest is index 0. Duplicates collapse to the front. Capacity RECENTS_MAX.
    Writes are best-effort (read-only volumes such as xemu just keep the list
    in memory for the session).
---------------------------------------------------------------------------*/
#ifndef DD_RECENTS_H
#define DD_RECENTS_H

#ifdef __cplusplus
extern "C" {
#endif

#define RECENTS_MAX        5
#define RECENTS_NAME_MAX  64
#define RECENTS_PATH_MAX 272

    void        Recents_Init(void);                              /* load at boot */
    void        Recents_Add(const char* label, const char* xbePath);
    int         Recents_Count(void);
    const char* Recents_Name(int i);                             /* friendly label */
    const char* Recents_Path(int i);                             /* XBE dos path   */

#ifdef __cplusplus
}
#endif
#endif /* DD_RECENTS_H */