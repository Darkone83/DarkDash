/*---------------------------------------------------------------------------
    dd_hidden.h -- launcher "hide / exclude" list.

    A persistent set of XBE paths the user has chosen to hide from the launcher
    lists (handy for collapsing weird duplicate titles down to a single visible
    entry). Keyed by the full default.xbe path so two copies of the same title in
    different folders are distinguished -- hide one, the other still shows. Fully
    reversible: un-hide restores the entry.

    Persisted as a small versioned blob in D:\data\hidden.dat (sits alongside
    dc.dat / paths.dat, NOT in the disposable cache dir). C89 style, no CRT str*.
---------------------------------------------------------------------------*/
#ifndef DD_HIDDEN_H
#define DD_HIDDEN_H

#ifdef __cplusplus
extern "C" {
#endif

    /* Load the hidden list from disk (lazy; safe to call repeatedly). The query
       functions below auto-load on first use, so an explicit call is optional. */
    void Hidden_Init(void);

    /* 1 if this default.xbe path is currently hidden. Case-insensitive match. */
    int  Hidden_Is(const char* xbePath);

    /* Hide (hide!=0) or un-hide (hide==0) a path, then persist immediately.
       Returns 1 on success (including "already in that state"); 0 only if the
       hide list is full or the write failed. */
    int  Hidden_Set(const char* xbePath, int hide);

    /* How many paths are currently hidden (across all categories). */
    int  Hidden_Count(void);

#ifdef __cplusplus
}
#endif

#endif /* DD_HIDDEN_H */