#ifndef DD_TRACE_H
#define DD_TRACE_H
/*---------------------------------------------------------------------------
    dd_trace.h -- brute-force "last breath" disk tracer.

    No debug console on the target, and a hard lock leaves no stack -- so the
    only way to see the last thing that ran before a freeze is a breadcrumb
    trail that is on the physical disk BEFORE the next instruction executes.

    Every Trace() call opens, appends one line, and CLOSES the file. The close
    forces the flush, so when the box freezes the file's final line is the last
    thing that ran. This is deliberately slow (a full open/write/close per call)
    -- that is the price of a trustworthy timeline. Turn it off (DD_TRACE_ON 0)
    once the culprit is found.

    Each line: <tick> <thread> <tag> <msg>
      tick   = GetTickCount() ms (wraps ~49 days; fine for a session)
      thread = which thread logged: MAIN / SVC / FILL / WDOG / ? (by thread id)

    Output: D:\data\ddtrace.log  (append; delete it between runs to keep it short)
---------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C" {
#endif

#define DD_TRACE_ON 0   /* 0 = tracer compiled out (no code, no log). Set to 1 to re-arm disk tracing. */

#if DD_TRACE_ON
    void Trace_Init(void);                       /* register the main thread id; truncate the log */
    void Trace_Thread(const char* name);         /* label the CURRENT thread (call once at thread start) */
    void Trace(const char* tag, const char* msg);/* CACHED line (fast): "<tag> <msg>" */
    void TraceU(const char* tag, const char* msg, unsigned long v); /* cached line + a decimal */
    void TraceD(const char* tag, const char* msg);/* DURABLE line (write-through): survives a hard lock */
    void TraceDU(const char* tag, const char* msg, unsigned long v);/* durable line + a decimal */
#  define TRACE(tag, msg)        Trace((tag), (msg))
#  define TRACEU(tag, msg, v)    TraceU((tag), (msg), (unsigned long)(v))
#  define TRACED(tag, msg)       TraceD((tag), (msg))
#  define TRACEDU(tag, msg, v)   TraceDU((tag), (msg), (unsigned long)(v))
#  define TRACE_THREAD(name)     Trace_Thread((name))
#else
#  define Trace_Init()           ((void)0)
#  define TRACE(tag, msg)        ((void)0)
#  define TRACEU(tag, msg, v)    ((void)0)
#  define TRACED(tag, msg)       ((void)0)
#  define TRACEDU(tag, msg, v)   ((void)0)
#  define TRACE_THREAD(name)     ((void)0)
#endif

#ifdef __cplusplus
}
#endif
#endif /* DD_TRACE_H */