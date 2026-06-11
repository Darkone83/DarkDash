/*---------------------------------------------------------------------------
    dd_trace.cpp -- see dd_trace.h.

    Build: MSVC2003/C89 style; file-scope statics; no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_trace.h"

#if DD_TRACE_ON

#define DD_TRACE_FILE "D:\\data\\ddtrace.log"

/* WRITE_THROUGH may not be in the RXDK headers; define it if missing. */
#ifndef FILE_FLAG_WRITE_THROUGH
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#endif

/* Up to 8 named threads. We map GetCurrentThreadId() -> a short label so each
   line says who ran. Registration is first-come; the table is tiny and only
   written at thread start, read under no lock (benign: worst case a line shows
   '?' for one entry during the brief registration window). */
#define MAXTHREADS 8
static DWORD       s_tid[MAXTHREADS];
static const char* s_tname[MAXTHREADS];
static int         s_nThreads = 0;
static CRITICAL_SECTION s_cs;
static int         s_ready = 0;

/* unsigned -> decimal (no CRT) */
static int U2A(unsigned long v, char* out) {
    char tmp[16]; int n = 0, i = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 15) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) out[i++] = tmp[--n];
    out[i] = 0;
    return i;
}

static const char* ThreadName(void) {
    DWORD id = GetCurrentThreadId();
    int i;
    for (i = 0; i < s_nThreads; i++)
        if (s_tid[i] == id) return s_tname[i];
    return "?";
}

void Trace_Init(void) {
    HANDLE f;
    if (!s_ready) { InitializeCriticalSection(&s_cs); s_ready = 1; }
    CreateDirectoryA("D:\\data", NULL);
    /* truncate any prior log so each session starts clean */
    f = CreateFileA(DD_TRACE_FILE, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    Trace_Thread("MAIN");
}

void Trace_Thread(const char* name) {
    DWORD id = GetCurrentThreadId();
    int i;
    if (!s_ready) return;
    EnterCriticalSection(&s_cs);
    for (i = 0; i < s_nThreads; i++) {
        if (s_tid[i] == id) { s_tname[i] = name; LeaveCriticalSection(&s_cs); return; }
    }
    if (s_nThreads < MAXTHREADS) {
        s_tid[s_nThreads] = id;
        s_tname[s_nThreads] = name;
        s_nThreads++;
    }
    LeaveCriticalSection(&s_cs);
}

/* The core: build one line and open/append/CLOSE. Two tiers:
     durable=0 (cached)  -- normal write; the kernel may hold it in write-back
                            cache. Fast. Used for the high-frequency firehose
                            (audio, music, svc, smbus) that is NOT the suspected
                            freeze point, so losing its last few lines is fine.
     durable=1 (through) -- FILE_FLAG_WRITE_THROUGH pushes the bytes past the
                            write-back cache to the drive on the write itself, so
                            a hard lock can't lose this line. Reserved for the
                            render/DrawSplash suspect path. Much cheaper than a
                            full FlushFileBuffers per line (which tanked the frame
                            rate), while still surviving the freeze.
   Serialized so interleaved threads don't garble a line. */
static void Emit(const char* tag, const char* msg, const unsigned long* num, int durable) {
    HANDLE f; DWORD wr; char line[200]; char n[16]; DWORD flags;
    if (!s_ready) return;
    EnterCriticalSection(&s_cs);

    line[0] = 0;
    U2A(GetTickCount(), n);  lstrcatA(line, n);
    lstrcatA(line, " ");     lstrcatA(line, ThreadName());
    lstrcatA(line, " ");     lstrcatA(line, tag ? tag : "");
    if (msg) { lstrcatA(line, " "); lstrcatA(line, msg); }
    if (num) { U2A(*num, n); lstrcatA(line, " "); lstrcatA(line, n); }
    lstrcatA(line, "\r\n");

    flags = FILE_ATTRIBUTE_NORMAL;
    if (durable) flags |= FILE_FLAG_WRITE_THROUGH;

    f = CreateFileA(DD_TRACE_FILE, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, flags, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        SetFilePointer(f, 0, NULL, FILE_END);
        WriteFile(f, line, (DWORD)lstrlenA(line), &wr, NULL);
        CloseHandle(f);
    }
    LeaveCriticalSection(&s_cs);
}

void Trace(const char* tag, const char* msg) { Emit(tag, msg, NULL, 0); }
void TraceU(const char* tag, const char* msg, unsigned long v) { Emit(tag, msg, &v, 0); }
void TraceD(const char* tag, const char* msg) { Emit(tag, msg, NULL, 1); }
void TraceDU(const char* tag, const char* msg, unsigned long v) { Emit(tag, msg, &v, 1); }

#endif /* DD_TRACE_ON */