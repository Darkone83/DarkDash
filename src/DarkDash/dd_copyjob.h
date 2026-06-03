/*---------------------------------------------------------------------------
    dd_copyjob.h -- asynchronous, cancellable copy/move of files & trees.

    A blocking Fileops_CopyTree freezes the dashboard for the whole transfer
    (no render, no cancel) and a big job can take minutes. A CopyJob does the
    same work incrementally: call CopyJob_Pump() once per frame so the UI keeps
    drawing a progress bar and can cancel with B.

    Memory is bounded by tree DEPTH, not file count -- the walk keeps a small
    stack of open find-handles and streams entries, so copying thousands of
    files uses the same handful of KB as copying one.

    Usage (driven from FileMan):
        CopyJob_Begin(items, nItems, destDir, isMove);
        ... each frame:
        st = CopyJob_Pump();              // RUNNING / DONE / FAILED / CANCELLED
        CopyJob_Progress(&filesDone, &filesTotalApprox, name, cap,
                         &curDone, &curTotal);
        ... on B:  CopyJob_Cancel();
---------------------------------------------------------------------------*/
#ifndef DD_COPYJOB_H
#define DD_COPYJOB_H

#ifdef __cplusplus
extern "C" {
#endif

#define COPYJOB_NAME_MAX 64
#define COPYJOB_PATH_MAX 256

    /* one top-level item to copy/move */
    typedef struct {
        char src[COPYJOB_PATH_MAX];     /* full source path  */
        char name[COPYJOB_NAME_MAX];    /* leaf name (placed under destDir) */
    } CopyJobItem;

    /* pump return states */
    enum { CJ_RUNNING = 0, CJ_DONE, CJ_FAILED, CJ_CANCELLED, CJ_IDLE };

    /* Start a job. 'items' are the top-level files/folders; each is copied to
       destDir\<name>. isMove=1 deletes each source after it fully copies. Returns 1
       if started, 0 on bad args. */
    int  CopyJob_Begin(const CopyJobItem* items, int nItems,
        const char* destDir, int isMove);

    /* Advance the job a little (call once per frame). Returns a CJ_* state. */
    int  CopyJob_Pump(void);

    /* Request cancel; the next pumps unwind cleanly and return CJ_CANCELLED. */
    void CopyJob_Cancel(void);

    /* 1 while a job is active (RUNNING). */
    int  CopyJob_Active(void);

    /* Progress snapshot. Any out-pointer may be NULL.
         filesDone        - files fully processed so far
         filesSeen        - files discovered so far (grows as the walk proceeds;
                            an approximate denominator, not known up front)
         curName/cap      - current file's leaf name
         curDone/curTotal - byte progress within the current file */
    void CopyJob_Progress(int* filesDone, int* filesSeen,
        char* curName, int cap,
        unsigned* curDone, unsigned* curTotal);

#ifdef __cplusplus
}
#endif
#endif /* DD_COPYJOB_H */