// FtpServ.cpp
// XbDiag FTP server — split from FileExplorer.cpp.
//
// Nonblocking XbDiag FTP engine; optionally pumped by a dedicated normal-priority
// Xbox worker so the data transfer no longer waits on D3D Present / frame pacing.
// The main thread exclusively owns worker start/stop; the worker exclusively owns
// sockets and file handles. Network restart synchronously joins the worker.
//
// Credentials : xbox / xbox   Port: 21   Mode: passive (PASV) only
// Commands    : USER PASS SYST TYPE PWD CWD CDUP LIST RETR STOR PASV QUIT
//               DELE MKD RMD RNFR RNTO SIZE NOOP FEAT MDTM
//               AUTH PBSZ PROT STAT REST ALLO MODE EPSV
//               (AUTH/PBSZ/PROT/EPSV/REST/ALLO acknowledged but not fully implemented)

#include "dd_ftp.h"
#include "input.h"        /* IsMUPresent */
#include "dd_fileops.h"
#include "dd_storage.h"
#include "dd_path.h"      /* shared path capacity */
#include "dd_data.h"      /* port + credentials */
#include "dd_net.h"       /* Net_Ip / Net_IsUp (deferred start) */
#include <xtl.h>
#include <winsockx.h>

/* local copy of XbDiag StrCopy (was in DiagCommon) */
static void StrCopy(char* dst, int dstLen, const char* src) {
    int i = 0;
    if (dstLen <= 0) return;
    while (src[i] && i < dstLen - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* local copies of XbDiag StrCat2 / IntToStr (were in DiagCommon) */
static void StrCat2(char* buf, int bufLen, const char* a, const char* b) {
    int i = 0;
    while (*a && i < bufLen - 1) buf[i++] = *a++;
    while (*b && i < bufLen - 1) buf[i++] = *b++;
    buf[i] = '\0';
}

static void IntToStr(int v, char* buf, int bufLen) {
    char tmp[16];
    int  out = 0, n = 0, i;
    unsigned u;
    if (bufLen <= 1) return;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    if (v < 0 && out < bufLen - 1) { buf[out++] = '-'; v = -v; }
    u = (unsigned)v;
    while (u > 0 && n < 15) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    for (i = n - 1; i >= 0 && out < bufLen - 1; --i) buf[out++] = tmp[i];
    buf[out] = '\0';
}

// ============================================================================
// Constants
// ============================================================================

/* control port + credentials come from dd_data (Data_Get()) */
#define FTP_DATA_PORT_BASE   2024
#define FTP_DATA_PORT_COUNT  32
#define FTP_MAX_PATH         DD_PATH_MAX    // max path length used in FTP operations
#define FTP_IDLE_TIMEOUT_MS  300000UL  // drop an idle control connection after 5 min

/* Upload buffering: 1=coalesce small TCP segments across service ticks,
   0=immediate FATX writes. Neither changes the TCP socket options. */
#ifndef DD_FTP_BATCHED_STOR
#define DD_FTP_BATCHED_STOR 1
#endif

   /* Experimental PrometheOS-inspired scheduling. 0 restores the original
      frame-polled FTP engine. No FTP command or path handling changes. */
#ifndef DD_FTP_WORKER_THREAD
#define DD_FTP_WORKER_THREAD 1
#endif

      /* The original batched patch used 64 KiB. PrometheOS uses 128 KiB disk
         batches; switch to 65536 for the earlier batch size. */
#ifndef DD_FTP_STOR_BLOCK_BYTES
#define DD_FTP_STOR_BLOCK_BYTES (128 * 1024)
#endif

         /* Incremental FATX reservation inspired by XBMC4Gamers. 0 keeps the working
            threaded/128 KiB upload engine with no changes to file allocation. */
#ifndef DD_FTP_PREALLOC_STOR
#define DD_FTP_PREALLOC_STOR 1
#endif
#define DD_FTP_PREALLOC_STEP_BYTES (2UL * 1024UL * 1024UL)

            /* PrometheOS requests a 4 MiB system file cache. OFF by default: it changes
               cache policy process-wide, so benchmark the worker independently first. */
#ifndef DD_FTP_USE_FILE_CACHE
#define DD_FTP_USE_FILE_CACHE 0
#endif

               // ============================================================================
               // Internal string helpers
               // ============================================================================

void FtpServ_AppendStr(char* out, int outLen, const char* src)
{
    int i = 0; while (out[i]) i++;
    while (*src && i < outLen - 1) out[i++] = *src++;
    out[i] = '\0';
}

// Copy src into dst, truncating to maxChars display characters.
// Appends "~" if truncated. dst must be >= dstLen bytes.
void FtpServ_TruncName(const char* src, char* dst, int maxChars, int dstLen)
{
    int len = 0; while (src[len]) len++;
    if (len <= maxChars)
    {
        int i = 0;
        while (*src && i < dstLen - 1) dst[i++] = *src++;
        dst[i] = '\0';
    }
    else
    {
        int i = 0;
        while (i < maxChars - 1 && i < dstLen - 2) dst[i++] = src[i];
        dst[i++] = '~';
        dst[i] = '\0';
    }
}

// ============================================================================
// Module state
// ============================================================================

FtpCtx g_ftp;

static int  s_nextDataPort = 0;
static char s_ipStr[20];
static bool s_ipOK = false;

/* Only the FTP service worker touches this memory; no producer/consumer
   races or per-frame partial-block flushes. Exactly one session is active. */
static char s_storBuf[DD_FTP_STOR_BLOCK_BYTES];
static int  s_storBuffered = 0;

/* Preallocation is transfer-local. Only the FTP worker touches these fields;
   committed progress remains g_ftp.xferDone (never the reserved EOF). */
#if DD_FTP_PREALLOC_STOR
static DWORD s_storInitialEnd = 0;
static DWORD s_storReservedEnd = 0;
static bool  s_storHasReservation = false;
static bool  s_storReserveDisabled = false;

static void FtpStorResetAllocation()
{
    s_storInitialEnd = 0;
    s_storReservedEnd = 0;
    s_storHasReservation = false;
    s_storReserveDisabled = false;
}

/* Passing a high part avoids treating positions above 2 GiB as negative.
   SetLastError is needed because 0xffffffff is a valid final byte offset. */
static bool FtpStorSeekAbsolute(HANDLE file, DWORD position)
{
    LONG highPart = 0;
    SetLastError(NO_ERROR);
    DWORD result = SetFilePointer(file, (LONG)position, &highPart, FILE_BEGIN);
    return highPart == 0 &&
        (result != 0xffffffffUL || GetLastError() == NO_ERROR);
}

static void FtpStorBeginAllocation(HANDLE file)
{
    FtpStorResetAllocation();
    DWORD highPart = 0;
    DWORD lowPart = GetFileSize(file, &highPart);
    if (highPart != 0) { s_storReserveDisabled = true; return; }
    s_storInitialEnd = lowPart;
    s_storReservedEnd = lowPart;
}

/* Reserve enough to cover the next write, rounded to 2 MiB. A reservation
   failure is only an optimization failure: restore the write pointer and
   continue with ordinary FATX extension. Do not change the TCP behavior. */
static bool FtpStorReserveForWrite(DWORD bytes)
{
    if (s_storReserveDisabled || bytes == 0) return true;
    const unsigned __int64 required =
        (unsigned __int64)g_ftp.xferDone + (unsigned __int64)bytes;
    if (required > (unsigned __int64)0xffffffffUL) return false; // FATX single-file limit
    if (required <= s_storReservedEnd) return true;

    unsigned __int64 rounded =
        ((required + DD_FTP_PREALLOC_STEP_BYTES - 1) /
            DD_FTP_PREALLOC_STEP_BYTES) * DD_FTP_PREALLOC_STEP_BYTES;
    if (rounded > (unsigned __int64)0xffffffffUL) rounded = (unsigned __int64)0xffffffffUL;

    const DWORD committedPos = g_ftp.xferDone;
    if (!FtpStorSeekAbsolute(g_ftp.xferFile, (DWORD)rounded))
    {
        s_storReserveDisabled = true;
        return FtpStorSeekAbsolute(g_ftp.xferFile, committedPos);
    }
    bool reserved = SetEndOfFile(g_ftp.xferFile) != 0;
    /* Track successful reservation *before* seeking back, so an unlikely
       seek-back failure still triggers abort-time EOF restoration. */
    if (reserved)
    {
        s_storReservedEnd = (DWORD)rounded;
        s_storHasReservation = true;
    }
    if (!FtpStorSeekAbsolute(g_ftp.xferFile, committedPos)) return false;
    if (!reserved) s_storReserveDisabled = true;
    return true;
}
#endif

/* Shared by clean EOF, ABOR, socket disconnect, QUIT and network restart.
   On interruption, preserve at least the file's original size, so a resumed
   upload does not accidentally destroy an existing suffix. On success,
   truncate to exactly the committed upload length (never the reserved EOF).
   Buffered but uncommitted network data are deliberately discarded on abort. */
static bool FtpStorCloseFile(bool success)
{
    bool ok = true;
    s_storBuffered = 0;
    if (g_ftp.xferFile != INVALID_HANDLE_VALUE)
    {
#if DD_FTP_PREALLOC_STOR
        if (g_ftp.xferType == XFER_STOR && s_storHasReservation)
        {
            DWORD finalLength = g_ftp.xferDone;
            if (!success && finalLength < s_storInitialEnd)
                finalLength = s_storInitialEnd;
            if (!FtpStorSeekAbsolute(g_ftp.xferFile, finalLength) ||
                !SetEndOfFile(g_ftp.xferFile))
                ok = false;
        }
#endif
        if (success && g_ftp.xferType == XFER_STOR &&
            !FlushFileBuffers(g_ftp.xferFile)) ok = false;
        if (!CloseHandle(g_ftp.xferFile)) ok = false;
        g_ftp.xferFile = INVALID_HANDLE_VALUE;
    }
#if DD_FTP_PREALLOC_STOR
    FtpStorResetAllocation();
#endif
    return ok;
}

#if DD_FTP_BATCHED_STOR
/* Advance progress only after writes succeed, handling partial writes.
   A 128 KiB staged block is one disk transaction in the normal case. */
static bool FtpStorCommitBuffered()
{
    DWORD done = 0;
    const DWORD total = (DWORD)s_storBuffered;
#if DD_FTP_PREALLOC_STOR
    if (!FtpStorReserveForWrite(total)) return false;
#endif
    while (done < total)
    {
        DWORD written = 0;
        if (!WriteFile(g_ftp.xferFile, s_storBuf + done, total - done,
            &written, NULL) || written == 0)
            return false;
        done += written;
        g_ftp.xferDone += written;
    }
    s_storBuffered = 0;
    return true;
}
#endif

// ============================================================================
// FTP helpers
// ============================================================================

static void FtpSendStr(SOCKET s, const char* str)
{
    // Queue into ctrl send buffer; FtpTick drains it each frame.
    if (s != g_ftp.ctrlSock) return;  // only used for ctrl replies
    int len = 0; while (str[len]) len++;

    // If the reply won't fit, first compact the buffer by shifting out the
    // already-sent prefix. sendOff bytes at the front are gone — reclaim them.
    int space = (int)sizeof(g_ftp.sendBuf) - g_ftp.sendLen;
    if (len > space && g_ftp.sendOff > 0)
    {
        int remaining = g_ftp.sendLen - g_ftp.sendOff;
        for (int i = 0; i < remaining; ++i)
            g_ftp.sendBuf[i] = g_ftp.sendBuf[g_ftp.sendOff + i];
        g_ftp.sendLen = remaining;
        g_ftp.sendOff = 0;
        space = (int)sizeof(g_ftp.sendBuf) - g_ftp.sendLen;
    }

    // After compaction, if it still won't fit the buffer is genuinely exhausted.
    // This should be unreachable in normal operation (8192 bytes, command intake
    // paused at threshold) — but if it happens, stay connected and discard rather
    // than drop the session. A missed reply is recoverable; a disconnect is not.
    if (len > space) return;

    for (int i = 0; i < len; ++i)
        g_ftp.sendBuf[g_ftp.sendLen++] = str[i];
}

static void FtpReply(int code, const char* msg)
{
    char line[128];
    char codeBuf[8];
    IntToStr(code, codeBuf, sizeof(codeBuf));
    line[0] = '\0';
    StrCat2(line, sizeof(line), line, codeBuf);
    StrCat2(line, sizeof(line), line, " ");
    StrCat2(line, sizeof(line), line, msg);
    StrCat2(line, sizeof(line), line, "\r\n");
    FtpSendStr(g_ftp.ctrlSock, line);
}

// Open a passive data listen socket and return the port
static bool FtpOpenPassive()
{
    if (g_ftp.dataListen != INVALID_SOCKET)
    {
        closesocket(g_ftp.dataListen);
        g_ftp.dataListen = INVALID_SOCKET;
    }
    if (g_ftp.dataSock != INVALID_SOCKET)
    {
        closesocket(g_ftp.dataSock);
        g_ftp.dataSock = INVALID_SOCKET;
    }

    g_ftp.dataListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_ftp.dataListen == INVALID_SOCKET) return false;

    // SO_REUSEADDR so rebind succeeds immediately after prior connection closes
    DWORD reuse = 1;
    setsockopt(g_ftp.dataListen, SOL_SOCKET, SO_REUSEADDR,
        (const char*)&reuse, sizeof(reuse));

    u_long nb = 1;
    ioctlsocket(g_ftp.dataListen, FIONBIO, &nb);

    // Rotate through port range to avoid TIME_WAIT on rapid successive transfers
    int port = FTP_DATA_PORT_BASE + (s_nextDataPort % FTP_DATA_PORT_COUNT);
    s_nextDataPort++;

    SOCKADDR_IN sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((u_short)port);

    if (bind(g_ftp.dataListen, (SOCKADDR*)&sa, sizeof(sa)) != 0 ||
        listen(g_ftp.dataListen, 1) != 0)
    {
        closesocket(g_ftp.dataListen);
        g_ftp.dataListen = INVALID_SOCKET;
        return false;
    }
    g_ftp.dataPort = (WORD)port;
    return true;
}

// Build PASV response using our IP and data port
static void FtpSendPasv()
{
    // Parse IP octets from s_ipStr
    BYTE oct[4] = { 127, 0, 0, 1 };
    if (s_ipOK)
    {
        const char* p = s_ipStr;
        for (int i = 0; i < 4; ++i)
        {
            int v = 0;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            oct[i] = (BYTE)v;
            if (*p == '.') p++;
        }
    }

    WORD port = g_ftp.dataPort;
    char reply[64];
    char nums[8];
    reply[0] = '\0';
    StrCat2(reply, sizeof(reply), reply, "227 Entering Passive Mode (");

    char o[4];
    for (int i = 0; i < 4; ++i)
    {
        IntToStr((int)oct[i], o, sizeof(o));
        StrCat2(reply, sizeof(reply), reply, o);
        StrCat2(reply, sizeof(reply), reply, ",");
    }
    IntToStr((int)(port >> 8), nums, sizeof(nums));
    StrCat2(reply, sizeof(reply), reply, nums);
    StrCat2(reply, sizeof(reply), reply, ",");
    IntToStr((int)(port & 0xFF), nums, sizeof(nums));
    StrCat2(reply, sizeof(reply), reply, nums);
    StrCat2(reply, sizeof(reply), reply, ").\r\n");
    FtpSendStr(g_ftp.ctrlSock, reply);
}

// Append one LIST line to g_ftp.listBuf. Returns false if buffer full.
// line format: "drwxr-xr-x  1 xbox xbox  <size> Jan 01 00:00 <n>\r\n"
static bool FtpAppendListLine(const char* name, bool isDir, DWORD sizeLow)
{
    char line[FTP_MAX_PATH + 64];
    char* p = line;

    const char* perm = isDir ? "drwxr-xr-x" : "-rw-r--r--";
    while (*perm) *p++ = *perm++;
    *p++ = ' ';

    const char* owner = "  1 xbox xbox ";
    while (*owner) *p++ = *owner++;

    char szBuf[12];
    IntToStr((int)sizeLow, szBuf, sizeof(szBuf));
    int szLen = 0; while (szBuf[szLen]) szLen++;
    for (int i = szLen; i < 10; ++i) *p++ = ' ';
    const char* sp = szBuf; while (*sp) *p++ = *sp++;
    *p++ = ' ';

    const char* dt = "Jan 01 00:00 ";
    while (*dt) *p++ = *dt++;

    const char* nm = name; while (*nm) *p++ = *nm++;
    *p++ = '\r'; *p++ = '\n'; *p = '\0';

    int lineLen = (int)(p - line);
    int space = (int)sizeof(g_ftp.listBuf) - g_ftp.listBufLen;
    if (lineLen > space) return false;  // buffer full — truncate listing
    for (int i = 0; i < lineLen; ++i)
        g_ftp.listBuf[g_ftp.listBufLen++] = line[i];
    return true;
}

void FtpServ_Start(const char* ipStr, bool ipOK)
{
    // Store network state from FileExplorer
    s_ipOK = ipOK;
    if (ipStr) { int _i = 0; while (ipStr[_i] && _i < 19) { s_ipStr[_i] = ipStr[_i]; ++_i; } s_ipStr[_i] = '\0'; }

    ZeroMemory(&g_ftp, sizeof(g_ftp));
    g_ftp.listenSock = INVALID_SOCKET;
    g_ftp.ctrlSock = INVALID_SOCKET;
    g_ftp.dataListen = INVALID_SOCKET;
    g_ftp.dataSock = INVALID_SOCKET;
    g_ftp.xferFile = INVALID_HANDLE_VALUE;
    s_nextDataPort = 0;  // reset port rotation on each server start

    if (!s_ipOK) return;  // no network

    g_ftp.listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_ftp.listenSock == INVALID_SOCKET) return;

    DWORD reuse = 1;
    setsockopt(g_ftp.listenSock, SOL_SOCKET, SO_REUSEADDR,
        (const char*)&reuse, sizeof(reuse));

    u_long nb = 1;
    ioctlsocket(g_ftp.listenSock, FIONBIO, &nb);

    SOCKADDR_IN sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    {
        int port = Data_Get()->ftpPort;
        if (port < 1 || port > 65535) port = 21;
        sa.sin_port = htons((u_short)port);
    }

    if (bind(g_ftp.listenSock, (SOCKADDR*)&sa, sizeof(sa)) != 0 ||
        listen(g_ftp.listenSock, 5) != 0)
    {
        closesocket(g_ftp.listenSock);
        g_ftp.listenSock = INVALID_SOCKET;
        return;
    }

    // Always start in virtual root — MapFtpPath converts at filesystem boundaries
    g_ftp.atVirtualRoot = true;
    StrCopy(g_ftp.cwd, sizeof(g_ftp.cwd), "/");
    g_ftp.state = FTP_LISTEN;
}

// Stop FTP server — close all sockets
static void FtpServ_StopEngine()
{
    FtpStorCloseFile(false);
    if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
    if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
    if (g_ftp.ctrlSock != INVALID_SOCKET) { closesocket(g_ftp.ctrlSock);   g_ftp.ctrlSock = INVALID_SOCKET; }
    if (g_ftp.listenSock != INVALID_SOCKET) { closesocket(g_ftp.listenSock); g_ftp.listenSock = INVALID_SOCKET; }
    g_ftp.state = FTP_OFF;
}

// Clean a virtual FTP path.
// - Splits on '/' and '\\', processes each component.
// - Skips empty components and ".".
// - ".." pops the last component (clamped at root, cannot escape).
// - Rejects components containing Windows-illegal chars (* ? " < > |).
// - Accepts ANY other name: _folder, .hidden, __temp, F:, etc.
// - Result always starts with '/' and has no trailing '/'.
static int CleanVirtualPath(const char* virtualPath, char* out, int outLen)
{
    int  oi = 0;
    const char* p = virtualPath;

    if (!out || outLen < 2) return 0;
    out[0] = '\0';

    while (*p)
    {
        // Skip separators
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;

        // Extract next component. Refuse an overlong component rather than
        // silently converting it into a different path.
        char tok[FTP_MAX_PATH];
        int  ti = 0;
        while (*p && *p != '/' && *p != '\\')
        {
            if (ti >= (int)sizeof(tok) - 1) return 0;
            tok[ti++] = *p++;
        }
        tok[ti] = '\0';

        if (tok[0] == '\0')
            continue;                               // empty — skip

        if (tok[0] == '.' && tok[1] == '\0')
            continue;                               // "." — skip

        if (tok[0] == '.' && tok[1] == '.' && tok[2] == '\0')
        {
            // ".." — pop last component, clamped at root
            if (oi > 0)
            {
                oi--;
                while (oi > 0 && out[oi] != '/') oi--;
            }
            continue;
        }

        // Reject Windows-illegal chars (colon is allowed: needed for "F:")
        for (int i = 0; tok[i]; i++)
        {
            char c = tok[i];
            if (c == '*' || c == '?' || c == '"' ||
                c == '<' || c == '>' || c == '|')
                return 0;
        }

        // Append /component exactly; never truncate.
        if (oi + 1 + ti >= outLen) return 0;
        out[oi++] = '/';
        for (int i = 0; tok[i]; i++) out[oi++] = tok[i];
    }

    if (oi == 0) { out[0] = '/'; out[1] = '\0'; return 1; }
    out[oi] = '\0';
    return 1;
}


static int ResolveRelative(const char* cwd, const char* arg, char* out, int outLen)
{
    if (arg[0] != '/') {
        // Max accepted cwd + separator + arg fits exactly in this scratch buffer.
        char tmp[FTP_MAX_PATH * 2];
        int need = DD_PathLen(cwd) + 1 + DD_PathLen(arg);
        if (need >= (int)sizeof(tmp)) return 0;
        StrCopy(tmp, sizeof(tmp), cwd);
        FtpServ_AppendStr(tmp, sizeof(tmp), "\\");
        FtpServ_AppendStr(tmp, sizeof(tmp), arg);
        return CleanVirtualPath(tmp, out, outLen);
    }
    // absolute virtual path — clean as-is
    return CleanVirtualPath(arg, out, outLen);
}

// Port of PrometheOS mapFtpPath (simplified for bare drive-letter virtual paths).
// "/F:/Apps/Type D" -> "F:\\Apps\\Type D"
// "/" -> "/"  (virtual root — caller must check)
static int MapFtpPath(const char* virt, char* out, int outLen)
{
    const char* a = (virt[0] == '/') ? virt + 1 : virt;
    int alen = DD_PathLen(a);

    if (!out || outLen <= 0) return 0;
    out[0] = '\0';

    // CerBIOS secondary disks are exposed as HDD1-E/HDD1-F/etc. Keep
    // the legacy /N: path working too, but advertise unambiguous names.
    {
        int vi;
        for (vi = 0; vi < DD_Storage_Count(); vi++) {
            const DD_StorageVolume* v = DD_Storage_At(vi);
            const char* alias = DD_Storage_FtpName(v);
            int n = 0, k = 0;
            if (!alias || !DD_Storage_Ready(v)) continue;
            while (alias[n] && a[n] &&
                ((alias[n] >= 'A' && alias[n] <= 'Z' ? alias[n] + 32 : alias[n]) ==
                    (a[n] >= 'A' && a[n] <= 'Z' ? a[n] + 32 : a[n]))) ++n;
            if (alias[n] || (a[n] && a[n] != '/' && a[n] != '\\')) continue;
            if (3 + DD_PathLen(a + n) >= outLen) return 0;
            out[k++] = v->letter; out[k++] = ':'; out[k++] = '\\';
            if (a[n] == '/' || a[n] == '\\') ++n;
            while (a[n]) { out[k++] = (a[n] == '/') ? '\\' : a[n]; ++n; }
            out[k] = 0;
            return 1;
        }
    }

    // Translate MU friendly names (P1-MMU1 etc.) to their drive letters.
    if ((a[0] == 'P') && (a[1] >= '1' && a[1] <= '4') && a[2] == '-' &&
        a[3] == 'M' && a[4] == 'M' && a[5] == 'U' && (a[6] == '1' || a[6] == '2'))
    {
        int port = a[1] - '1';
        int slot = a[6] - '1';
        char letter = Mu_Letter(port, slot);
        const char* rest = a + 7;
        int restLen;
        if (*rest == '/' || *rest == '\\') ++rest;
        restLen = DD_PathLen(rest);
        if (3 + restLen >= outLen) return 0;
        out[0] = letter; out[1] = ':'; out[2] = '\\'; out[3] = '\0';
        if (*rest)
        {
            int i = 3;
            while (*rest)
            {
                out[i++] = (*rest == '/') ? '\\' : *rest;
                ++rest;
            }
            out[i] = '\0';
        }
        return 1;
    }

    // Non-MU mapping is length preserving except "F:" -> "F:\\".
    if (alen >= outLen) return 0;
    if (!DD_PathCopy(out, outLen, a)) return 0;
    for (int i = 0; out[i]; i++) if (out[i] == '/') out[i] = '\\';
    if (alen == 2 && out[1] == ':')
    {
        if (3 >= outLen) { out[0] = '\0'; return 0; }
        out[2] = '\\'; out[3] = '\0';
    }
    return 1;
}

// Convenience: resolve arg against cwd then map to real Windows path.
static int FtpResolvePath(const char* arg, char* out, int outLen)
{
    char virt[FTP_MAX_PATH];
    if (!ResolveRelative(g_ftp.cwd, arg, virt, sizeof(virt))) return 0;
    return MapFtpPath(virt, out, outLen);
}
// Execute a pending LIST — enumerate and send directly to dataSock, reply 226.
// Build listing into listBuf then start async drain via XFER_LIST.
// Called from FtpTick once dataSock is accepted.
static void FtpDoListing()
{
    g_ftp.listBufLen = 0;
    g_ftp.listBufOff = 0;

    if (g_ftp.listVirtualRoot)
    {
        // D: is the dashboard installation volume, retained for legacy FTP.
        if (GetFileAttributesA("D:\\") != 0xFFFFFFFFUL)
            FtpAppendListLine("D:", true, 0);
        for (int di = 0; di < DD_Storage_Count(); ++di)
        {
            const DD_StorageVolume* v = DD_Storage_At(di);
            if (!DD_Storage_Ready(v)) continue;
            // Display the same DOS drive letters as the file manager.
            // MapFtpPath continues accepting saved HDD1-E/F/G/C aliases.
            char driveName[3] = { v->letter, ':', 0 };
            FtpAppendListLine(driveName, true, 0);
        }
        // Memory Units
        for (int mu = 0; mu < 8; ++mu)
        {
            int port = mu / 2, slot = mu % 2;
            if (!IsMUPresent(port, slot)) continue;
            char muPat[8];
            muPat[0] = Mu_Letter(port, slot); muPat[1] = ':'; muPat[2] = '\\'; muPat[3] = '\0';
            // Use GetFileAttributesA — FindFirstFile("X:\\*") fails on empty MUs
            DWORD attr = GetFileAttributesA(muPat);
            if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY))
            {
                static const char* k_ftpMuNames[8] = {
                    "P1-MMU1","P1-MMU2","P2-MMU1","P2-MMU2",
                    "P3-MMU1","P3-MMU2","P4-MMU1","P4-MMU2" };
                FtpAppendListLine(k_ftpMuNames[mu], true, 0);
            }
        }
    }
    else
    {
        char pat[FTP_MAX_PATH + 4];
        StrCopy(pat, sizeof(pat), g_ftp.listDir);
        int pl = 0; while (pat[pl]) pl++;
        if (pl > 0 && pat[pl - 1] != '\\') { pat[pl] = '\\'; pat[pl + 1] = '\0'; pl++; }
        pat[pl] = '*'; pat[pl + 1] = '\0';

        WIN32_FIND_DATA fd;
        HANDLE h = FindFirstFile(pat, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do {
                if (fd.cFileName[0] == '.' && (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;
                bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                FtpAppendListLine(fd.cFileName, isDir, isDir ? 0 : fd.nFileSizeLow);
            } while (FindNextFile(h, &fd));
            FindClose(h);
        }
    }

    // Listing built — send 150 and start async drain
    FtpReply(150, "Opening data connection.");
    g_ftp.listPending = false;
    g_ftp.xferType = XFER_LIST;
    g_ftp.state = FTP_TRANSFER;
}

// Recursively delete a directory and all its contents
static bool FtpDeleteDir(const char* path)
{
    char pat[DD_PATH_PATTERN_MAX];
    if (!DD_PathJoin(pat, sizeof(pat), path, "*")) return false;

    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pat, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do {
            if (fd.cFileName[0] == '.' && (fd.cFileName[1] == '\0' || fd.cFileName[1] == '.')) continue;

            // Build full child path
            char child[FTP_MAX_PATH];
            if (!DD_PathJoin(child, sizeof(child), path, fd.cFileName)) {
                FindClose(h);
                return false;
            }

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!FtpDeleteDir(child)) { FindClose(h); return false; }
            }
            else
            {
                if (!DeleteFileA(child)) { FindClose(h); return false; }
            }
        } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(path) != 0;
}

// Process one complete FTP command line (null-terminated, no \r\n)
static void FtpHandleCommand(char* cmd)
{
    // Split verb and argument
    char verb[16] = {};
    char arg[1024] = {};
    int i = 0;
    while (cmd[i] && cmd[i] != ' ' && i < 15) { verb[i] = cmd[i]; i++; }
    verb[i] = '\0';
    if (cmd[i] == ' ') {
        int ai = 0;
        i++;
        while (cmd[i + ai]) {
            if (ai >= (int)sizeof(arg) - 1) { FtpReply(501, "Argument too long."); return; }
            arg[ai] = cmd[i + ai];
            ai++;
        }
        arg[ai] = '\0';
    }

    // Strip surrounding quotes — some clients quote paths containing spaces
    // e.g. CWD "F:\Apps\Type D"
    {
        int alen = 0; while (arg[alen]) alen++;
        if (alen >= 2 && arg[0] == '"' && arg[alen - 1] == '"')
        {
            for (int k = 0; k < alen - 2; ++k) arg[k] = arg[k + 1];
            arg[alen - 2] = '\0';
        }
    }

    // Uppercase verb
    for (int k = 0; verb[k]; ++k)
        if (verb[k] >= 'a' && verb[k] <= 'z') verb[k] -= 32;

    if (!g_ftp.authed)
    {
        // Only USER and PASS allowed before auth
        if (verb[0] == 'U' && verb[1] == 'S' && verb[2] == 'E' && verb[3] == 'R')
        {
            // Check username: "xbox"
            bool ok = (lstrcmpA(arg, Data_Get()->ftpUser) == 0);
            g_ftp.gotUser = ok;
            FtpReply(331, "Password required.");
        }
        else if (verb[0] == 'P' && verb[1] == 'A' && verb[2] == 'S' && verb[3] == 'S')
        {
            bool ok = g_ftp.gotUser &&
                (lstrcmpA(arg, Data_Get()->ftpPass) == 0);
            if (ok) { g_ftp.authed = true; g_ftp.atVirtualRoot = true; StrCopy(g_ftp.cwd, sizeof(g_ftp.cwd), "/"); FtpReply(230, "Logged in."); }
            else { FtpReply(530, "Login incorrect."); }
        }
        else if (verb[0] == 'Q' && verb[1] == 'U' && verb[2] == 'I' && verb[3] == 'T')
        {
            FtpReply(221, "Bye.");
            closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
            g_ftp.state = FTP_LISTEN;
        }
        else { FtpReply(530, "Not logged in."); }
        return;
    }

    // ---- Authenticated commands ----
    if (verb[0] == 'S' && verb[1] == 'Y' && verb[2] == 'S' && verb[3] == 'T')
    {
        FtpReply(215, "Windows_NT");
    }
    else if (verb[0] == 'T' && verb[1] == 'Y' && verb[2] == 'P' && verb[3] == 'E')
    {
        FtpReply(200, "Type set.");
    }
    else if ((verb[0] == 'P' && verb[1] == 'W' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'P' && verb[2] == 'W' && verb[3] == 'D'))
    {
        // PWD returns virtual path exactly — same as PrometheOS
        char reply[FTP_MAX_PATH + 8];
        reply[0] = '"';
        int ri = 1;
        const char* pp = g_ftp.cwd;
        while (*pp && ri < (int)sizeof(reply) - 4) reply[ri++] = *pp++;
        reply[ri++] = '"'; reply[ri++] = '\0';
        FtpReply(257, reply);
    }
    else if ((verb[0] == 'C' && verb[1] == 'W' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'W' && verb[3] == 'D') ||
        (verb[0] == 'C' && verb[1] == 'D' && verb[2] == 'U' && verb[3] == 'P') ||
        (verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'U' && verb[3] == 'P'))
    {
        // Exact port of PrometheOS CWD/CDUP logic.
        bool isCdup = (verb[0] == 'C' && verb[1] == 'D' && verb[2] == 'U') ||
            (verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'U');
        const char* param = isCdup ? ".." : arg;

        char newVirtual[FTP_MAX_PATH];
        if (!ResolveRelative(g_ftp.cwd, param, newVirtual, sizeof(newVirtual))) {
            FtpReply(553, "Path too long or invalid."); return;
        }

        bool isFolder = false;

        // Case 1: resolved to virtual root "/"
        if (newVirtual[0] == '/' && newVirtual[1] == '\0')
        {
            isFolder = true;
        }
        else
        {
            char ftpPath[FTP_MAX_PATH];
            if (!MapFtpPath(newVirtual, ftpPath, sizeof(ftpPath))) {
                FtpReply(553, "Path too long or invalid."); return;
            }

            // Case 2: drive root — ftpPath is "F:" or "F:\"
            int fplen = 0; while (ftpPath[fplen]) fplen++;
            if (fplen >= 2 && ftpPath[1] == ':' && (ftpPath[2] == '\0' || (ftpPath[2] == '\\' && ftpPath[3] == '\0')))
            {
                isFolder = true;
            }
            else
            {
                // Case 3: subdirectory.
                // GetFileAttributesA first — works on non-empty dirs.
                // Fallback: try CreateDirectoryA; if it fails with
                // ERROR_ALREADY_EXISTS the path exists and is a directory.
                // This handles empty directories on RXDK where GetFileAttributes
                // may return INVALID_FILE_ATTRIBUTES.
                DWORD attr = GetFileAttributesA(ftpPath);
                if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    isFolder = true;
                }
                else
                {
                    if (!CreateDirectoryA(ftpPath, NULL) &&
                        GetLastError() == ERROR_ALREADY_EXISTS)
                        isFolder = true;
                }
            }
        }

        if (isFolder)
        {
            g_ftp.atVirtualRoot = (newVirtual[0] == '/' && newVirtual[1] == '\0');
            StrCopy(g_ftp.cwd, sizeof(g_ftp.cwd), newVirtual);
            FtpReply(250, "Directory changed.");
        }
        else
        {
            FtpReply(550, "No such directory.");
        }
    }
    else if (verb[0] == 'P' && verb[1] == 'A' && verb[2] == 'S' && verb[3] == 'V')
    {
        if (FtpOpenPassive()) FtpSendPasv();
        else FtpReply(425, "Cannot open data connection.");
    }
    else if ((verb[0] == 'L' && verb[1] == 'I' && verb[2] == 'S' && verb[3] == 'T') ||
        (verb[0] == 'N' && verb[1] == 'L' && verb[2] == 'S' && verb[3] == 'T'))
    {
        if (g_ftp.dataListen == INVALID_SOCKET)
        {
            FtpReply(425, "Use PASV first."); return;
        }

        // Strip all leading flag groups (e.g. "-a", "-la", "-al", "-a -l")
        // Some clients send multiple flag tokens before the path or nothing.
        char* listArg = arg;
        while (listArg[0] == '-')
        {
            while (*listArg && *listArg != ' ') listArg++;  // skip flag token
            while (*listArg == ' ') listArg++;              // skip spaces
        }

        // Resolve listing target.
        // Empty arg or bare "/" after flag stripping always means current directory.
        // At virtual root that produces the drive letter listing.
        bool isRootArg = (listArg[0] == '\0') ||
            (listArg[0] == '/' && listArg[1] == '\0');
        bool listVirtualRoot = g_ftp.atVirtualRoot && isRootArg;
        char listDir[FTP_MAX_PATH] = {};
        if (!listVirtualRoot)
        {
            if (listArg[0] != '\0')
            {
                if (!FtpResolvePath(listArg, listDir, sizeof(listDir))) {
                    FtpReply(553, "Path too long or invalid."); return;
                }
            }
            else
            {
                if (!MapFtpPath(g_ftp.cwd, listDir, sizeof(listDir))) {
                    FtpReply(553, "Path too long or invalid."); return;
                }
            }
        }

        // Store as pending then dispatch immediately if dataSock is already
        // accepted (fast clients connect to the data port before LIST arrives).
        // Otherwise FtpTick dispatches it when accept fires.
        g_ftp.listPending = true;
        g_ftp.listVirtualRoot = listVirtualRoot;
        StrCopy(g_ftp.listDir, sizeof(g_ftp.listDir), listDir);

        if (g_ftp.dataSock != INVALID_SOCKET)
            FtpDoListing();
    }
    else if (verb[0] == 'R' && verb[1] == 'E' && verb[2] == 'T' && verb[3] == 'R')
    {
        if (g_ftp.dataListen == INVALID_SOCKET)
        {
            FtpReply(425, "Use PASV first."); return;
        }

        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }

        HANDLE hf = CreateFile(fullPath, GENERIC_READ, FILE_SHARE_READ,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE)
        {
            FtpReply(550, "File not found."); return;
        }

        g_ftp.xferFile = hf;
        g_ftp.xferTotal = GetFileSize(hf, NULL);
        g_ftp.xferDone = 0;
        g_ftp.retrBufLen = 0;
        g_ftp.retrBufOff = 0;
        // REST resume: seek the source file to the requested offset so the
        // client can continue an interrupted download.
        if (g_ftp.restOffset > 0)
        {
            if (g_ftp.restOffset <= g_ftp.xferTotal)
            {
                SetFilePointer(hf, (LONG)g_ftp.restOffset, NULL, FILE_BEGIN);
                g_ftp.xferDone = g_ftp.restOffset;
            }
            g_ftp.restOffset = 0;   // consumed
        }
        FtpServ_TruncName(arg, g_ftp.xferName, 18, sizeof(g_ftp.xferName));
        StrCopy(g_ftp.xferPath, sizeof(g_ftp.xferPath), fullPath);
        // Defer 150 until dataSock is accepted — mirrors LIST's listPending pattern.
        // Sending 150 before the data connection exists starts the client's transfer
        // timeout prematurely; on back-to-back transfers the port may not be ready yet.
        g_ftp.retrPending = true;
        g_ftp.xferType = XFER_RETR;
        // Do not set FTP_TRANSFER yet — tick promotes state when dataSock accepted
        if (g_ftp.dataSock != INVALID_SOCKET)
        {
            FtpReply(150, "Opening data connection.");
            g_ftp.retrPending = false;
            g_ftp.state = FTP_TRANSFER;
        }
    }
    else if (verb[0] == 'S' && verb[1] == 'T' && verb[2] == 'O' && verb[3] == 'R')
    {
        if (g_ftp.dataListen == INVALID_SOCKET)
        {
            FtpReply(425, "Use PASV first."); return;
        }

        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }

        // ALLO pre-check: if the client declared a size and the target volume
        // can't hold it, refuse now rather than filling the disk and failing
        // mid-upload. Best-effort (only when ALLO was sent and the path has a
        // drive root we can query).
        if (g_ftp.alloSize > 0 && fullPath[1] == ':')
        {
            char root[4]; ULARGE_INTEGER freeAvail, totalB, totalFree;
            root[0] = fullPath[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
            if (GetDiskFreeSpaceExA(root, &freeAvail, &totalB, &totalFree))
            {
                if (freeAvail.QuadPart < (unsigned __int64)g_ftp.alloSize)
                {
                    g_ftp.alloSize = 0;
                    FtpReply(552, "Insufficient storage for upload.");
                    return;
                }
            }
        }
        g_ftp.alloSize = 0;   // consumed (or unusable)

        // REST resume on upload: open existing + seek; otherwise truncate.
        HANDLE hf;
        if (g_ftp.restOffset > 0)
        {
            hf = CreateFile(fullPath, GENERIC_WRITE, 0,
                NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE)
            {
#if DD_FTP_PREALLOC_STOR
                if (!FtpStorSeekAbsolute(hf, g_ftp.restOffset))
                {
                    CloseHandle(hf);
                    hf = INVALID_HANDLE_VALUE;
                }
#else
                SetFilePointer(hf, (LONG)g_ftp.restOffset, NULL, FILE_BEGIN);
#endif
            }
        }
        else
        {
            hf = CreateFile(fullPath, GENERIC_WRITE, 0,
                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (hf == INVALID_HANDLE_VALUE)
        {
            g_ftp.restOffset = 0;
            FtpReply(550, "Cannot create file."); return;
        }

        g_ftp.xferFile = hf;
        s_storBuffered = 0; // reset even after abort/reconnect or REST resume
#if DD_FTP_PREALLOC_STOR
        FtpStorBeginAllocation(hf);
#endif
        g_ftp.xferTotal = 0;   // unknown until complete
        g_ftp.xferDone = (g_ftp.restOffset > 0) ? g_ftp.restOffset : 0;
        g_ftp.restOffset = 0;   // consumed
        FtpServ_TruncName(arg, g_ftp.xferName, 18, sizeof(g_ftp.xferName));
        StrCopy(g_ftp.xferPath, sizeof(g_ftp.xferPath), fullPath);
        // Defer 150 until dataSock is accepted — same pattern as RETR/LIST.
        g_ftp.storPending = true;
        g_ftp.xferType = XFER_STOR;
        // Do not set FTP_TRANSFER yet — tick promotes state when dataSock accepted
        if (g_ftp.dataSock != INVALID_SOCKET)
        {
            FtpReply(150, "Opening data connection.");
            g_ftp.storPending = false;
            g_ftp.state = FTP_TRANSFER;
        }
    }
    else if (verb[0] == 'Q' && verb[1] == 'U' && verb[2] == 'I' && verb[3] == 'T')
    {
        FtpReply(221, "Bye.");
        FtpStorCloseFile(false);
        if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
        if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
        closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
        g_ftp.state = FTP_LISTEN;
        g_ftp.authed = false;
        g_ftp.gotUser = false;
        g_ftp.gotRnfr = false;
        g_ftp.atVirtualRoot = false;
        g_ftp.ctrlHalfClosed = false;
        g_ftp.listPending = false;
        g_ftp.retrPending = false;
        g_ftp.storPending = false;
        g_ftp.xferType = XFER_NONE;
        g_ftp.recvLen = 0;
        g_ftp.listBufLen = 0;
        g_ftp.listBufOff = 0;
        g_ftp.sendLen = 0;
        g_ftp.sendOff = 0;
    }
    else if (verb[0] == 'D' && verb[1] == 'E' && verb[2] == 'L' && verb[3] == 'E')
    {
        // Delete a file
        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        if (DeleteFileA(fullPath))
            FtpReply(250, "File deleted.");
        else
            FtpReply(550, "Delete failed.");
    }
    else if ((verb[0] == 'M' && verb[1] == 'K' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'M' && verb[2] == 'K' && verb[3] == 'D'))
    {
        // Create directory
        char newVirt[FTP_MAX_PATH];
        if (!ResolveRelative(g_ftp.cwd, arg, newVirt, sizeof(newVirt))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        char fullPath[FTP_MAX_PATH];
        if (!MapFtpPath(newVirt, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        if (CreateDirectoryA(fullPath, NULL))
        {
            // RFC 959: 257 response quotes the virtual path
            char reply[FTP_MAX_PATH + 4];
            reply[0] = '"';
            int ri = 1;
            const char* pp = newVirt;
            while (*pp && ri < (int)sizeof(reply) - 3) reply[ri++] = *pp++;
            reply[ri++] = '"'; reply[ri] = '\0';
            FtpReply(257, reply);
        }
        else
            FtpReply(550, "Cannot create directory.");
    }
    else if ((verb[0] == 'R' && verb[1] == 'M' && verb[2] == 'D') ||
        (verb[0] == 'X' && verb[1] == 'R' && verb[2] == 'M' && verb[3] == 'D'))
    {
        // Remove directory and all contents recursively
        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        if (FtpDeleteDir(fullPath))
            FtpReply(250, "Directory removed.");
        else
            FtpReply(550, "Remove failed.");
    }
    else if (verb[0] == 'R' && verb[1] == 'N' && verb[2] == 'F' && verb[3] == 'R')
    {
        // Rename from — verify source exists before storing
        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            g_ftp.gotRnfr = false;
            FtpReply(553, "Path too long or invalid."); return;
        }

        // Check as file first, then as directory
        DWORD attr = GetFileAttributesA(fullPath);
        if (attr == 0xFFFFFFFF)
        {
            g_ftp.gotRnfr = false;
            FtpReply(550, "No such file or directory.");
        }
        else
        {
            StrCopy(g_ftp.rnfrPath, sizeof(g_ftp.rnfrPath), fullPath);
            g_ftp.gotRnfr = true;
            FtpReply(350, "Waiting for RNTO.");
        }
    }
    else if (verb[0] == 'R' && verb[1] == 'N' && verb[2] == 'T' && verb[3] == 'O')
    {
        if (!g_ftp.gotRnfr)
        {
            FtpReply(503, "Send RNFR first."); return;
        }
        g_ftp.gotRnfr = false;

        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        if (MoveFileA(g_ftp.rnfrPath, fullPath))
            FtpReply(250, "Renamed.");
        else
            FtpReply(550, "Rename failed.");
    }
    else if (verb[0] == 'F' && verb[1] == 'E' && verb[2] == 'A' && verb[3] == 'T')
    {
        // Advertise supported features. Do NOT advertise MLST/MLSD — FileZilla
        // will prefer MLSD over LIST if advertised, and we don't implement it.
        // EPSV deliberately omitted — clients probe it anyway but we decline it.
        FtpSendStr(g_ftp.ctrlSock,
            "211-Features:\r\n"
            " SIZE\r\n"
            " MDTM\r\n"
            " REST STREAM\r\n"
            " UTF8\r\n"
            " TVFS\r\n"
            "211 END\r\n");
    }
    else if (verb[0] == 'O' && verb[1] == 'P' && verb[2] == 'T' && verb[3] == 'S')
    {
        // FileZilla sends "OPTS UTF8 ON" — just accept it
        FtpReply(200, "OK.");
    }
    else if (verb[0] == 'C' && verb[1] == 'L' && verb[2] == 'N' && verb[3] == 'T')
    {
        // FileZilla identifies itself with CLNT — accept silently
        FtpReply(200, "OK.");
    }
    else if (verb[0] == 'M' && verb[1] == 'L' && verb[2] == 'S' && verb[3] == 'D')
    {
        // FileZilla prefers MLSD over LIST — not implemented, but reply 502
        // so FileZilla retries with LIST rather than treating it as fatal.
        FtpReply(502, "MLSD not implemented, use LIST.");
    }
    else if (verb[0] == 'E' && verb[1] == 'P' && verb[2] == 'S' && verb[3] == 'V')
    {
        // FileZilla tries EPSV before PASV — decline with 500 so it falls back cleanly.
        // 522 means "network protocol not supported" which confuses some clients.
        FtpReply(500, "EPSV not supported, use PASV.");
    }
    else if (verb[0] == 'A' && verb[1] == 'B' && verb[2] == 'O' && verb[3] == 'R')
    {
        // Abort current transfer if any
        s_storBuffered = 0; // queued bytes never belong to a subsequent STOR
        FtpStorCloseFile(false);
        if (g_ftp.dataSock != INVALID_SOCKET)
        {
            closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
        }
        g_ftp.xferType = XFER_NONE;
        g_ftp.listPending = false;
        g_ftp.listBufLen = 0; g_ftp.listBufOff = 0;
        g_ftp.retrBufLen = 0; g_ftp.retrBufOff = 0;
        if (g_ftp.state == FTP_TRANSFER) g_ftp.state = FTP_CONNECTED;
        FtpReply(226, "Abort successful.");
    }
    else if (verb[0] == 'N' && verb[1] == 'O' && verb[2] == 'O' && verb[3] == 'P')
    {
        FtpReply(200, "OK.");
    }
    else if (verb[0] == 'S' && verb[1] == 'I' && verb[2] == 'Z' && verb[3] == 'E')
    {
        char fullPath[FTP_MAX_PATH];
        if (!FtpResolvePath(arg, fullPath, sizeof(fullPath))) {
            FtpReply(553, "Path too long or invalid."); return;
        }
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(fullPath, GetFileExInfoStandard, &fad) &&
            !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            // Report low 32 bits — files over 4GB are uncommon on Xbox FAT
            char szBuf[32];
            IntToStr((int)fad.nFileSizeLow, szBuf, sizeof(szBuf));
            FtpReply(213, szBuf);
        }
        else { FtpReply(550, "File not found."); }
    }
    else if (verb[0] == 'A' && verb[1] == 'U' && verb[2] == 'T' && verb[3] == 'H')
    {
        // WinSCP sends AUTH TLS — we are plain FTP only
        FtpReply(500, "AUTH not supported, plain FTP only.");
    }
    else if (verb[0] == 'P' && verb[1] == 'B' && verb[2] == 'S' && verb[3] == 'Z')
    {
        // WinSCP sends PBSZ 0 before PROT — acknowledge silently
        FtpReply(200, "PBSZ 0 OK.");
    }
    else if (verb[0] == 'P' && verb[1] == 'R' && verb[2] == 'O' && verb[3] == 'T')
    {
        // WinSCP sends PROT P — we only support clear data (PROT C)
        FtpReply(200, "PROT C assumed.");
    }
    else if (verb[0] == 'S' && verb[1] == 'T' && verb[2] == 'A' && verb[3] == 'T')
    {
        // WinSCP uses STAT as a connection keep-alive
        FtpReply(211, "DarkDash FTP Server ready.");
    }
    else if (verb[0] == 'M' && verb[1] == 'D' && verb[2] == 'T' && verb[3] == 'M')
    {
        // Return a dummy timestamp — Xbox FAT doesn't carry reliable mtime
        FtpReply(213, "19700101000000");
    }
    else if (verb[0] == 'R' && verb[1] == 'E' && verb[2] == 'S' && verb[3] == 'T')
    {
        // Resume support: remember the byte offset; the next RETR/STOR seeks to
        // it before transferring. Cleared after it's consumed (or on abort).
        DWORD off = 0;
        const char* p = arg;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { off = off * 10 + (DWORD)(*p - '0'); p++; }
        g_ftp.restOffset = off;
        FtpReply(350, "Restart position accepted, send RETR or STOR.");
    }
    else if (verb[0] == 'A' && verb[1] == 'L' && verb[2] == 'L' && verb[3] == 'O')
    {
        // Pre-allocation hint: capture the declared size so STOR can pre-check
        // free space and reject an upload that won't fit, rather than filling
        // the disk and failing mid-transfer. "ALLO <n>".
        DWORD sz = 0;
        const char* p = arg;
        while (*p == ' ') p++;
        while (*p >= '0' && *p <= '9') { sz = sz * 10 + (DWORD)(*p - '0'); p++; }
        g_ftp.alloSize = sz;
        FtpReply(200, "ALLO OK.");
    }
    else if (verb[0] == 'M' && verb[1] == 'O' && verb[2] == 'D' && verb[3] == 'E')
    {
        // MODE S (stream) is the only mode we support
        FtpReply(200, "MODE S OK.");
    }
    else if (verb[0] == 'S' && verb[1] == 'I' && verb[2] == 'T' && verb[3] == 'E')
    {
        // SITE subcommands — not implemented, 202 = "not implemented but accepted"
        // Use 202 not 502 so clients don't treat it as a fatal error
        FtpReply(202, "SITE not implemented.");
    }
    else if ((verb[0] == 'X' && verb[1] == 'C' && verb[2] == 'R' && verb[3] == 'C') ||
        (verb[0] == 'X' && verb[1] == 'M' && verb[2] == 'D' && verb[3] == '5'))
    {
        // FlashFXP checksum commands — not supported
        FtpReply(502, "Checksum commands not supported.");
    }
    else
    {
        FtpReply(502, "Command not implemented.");
    }
}

// ============================================================================
// FtpTick — called once per frame, non-blocking throughout
// ============================================================================

void FtpServ_Tick()
{
    if (g_ftp.state == FTP_OFF) return;

    // Idle timeout: drop a control connection that's sat silent too long, so a
    // dead/half-gone client doesn't hold the single session forever (the server
    // is single-connection, so a stuck client locks everyone else out). Only
    // while CONNECTED -- never during a transfer, where the control channel is
    // legitimately quiet for the whole download/upload.
    if (g_ftp.state == FTP_CONNECTED && g_ftp.ctrlSock != INVALID_SOCKET &&
        g_ftp.xferType == XFER_NONE)
    {
        if (GetTickCount() - g_ftp.lastActivityMs > FTP_IDLE_TIMEOUT_MS)
        {
            FtpSendStr(g_ftp.ctrlSock, "421 Idle timeout, closing control connection.\r\n");
            closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
            if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
            if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
            g_ftp.authed = false; g_ftp.gotUser = false; g_ftp.gotRnfr = false;
            g_ftp.atVirtualRoot = false; g_ftp.listPending = false;
            g_ftp.ctrlHalfClosed = false; g_ftp.retrPending = false; g_ftp.storPending = false;
            g_ftp.xferType = XFER_NONE; g_ftp.recvLen = 0;
            g_ftp.retrBufLen = 0; g_ftp.retrBufOff = 0;
            g_ftp.listBufLen = 0; g_ftp.listBufOff = 0;
            g_ftp.sendLen = 0; g_ftp.sendOff = 0;
            g_ftp.restOffset = 0; g_ftp.alloSize = 0;
            g_ftp.state = FTP_LISTEN;
            return;
        }
    }

    // ---- Drain ctrl send buffer ------------------------------------------
    // FtpSendStr() queues into sendBuf; we push bytes here each tick so
    // WSAEWOULDBLOCK never silently drops part of a reply.
    if (g_ftp.ctrlSock != INVALID_SOCKET && g_ftp.sendLen > g_ftp.sendOff)
    {
        int n = send(g_ftp.ctrlSock,
            g_ftp.sendBuf + g_ftp.sendOff,
            g_ftp.sendLen - g_ftp.sendOff, 0);
        if (n > 0)
        {
            g_ftp.sendOff += n;
            if (g_ftp.sendOff >= g_ftp.sendLen)
                g_ftp.sendOff = g_ftp.sendLen = 0;  // buffer fully drained
        }
        else if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
        {
            // Hard send error on ctrl socket — treat as disconnect
            closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
            FtpStorCloseFile(false);
            if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
            if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
            g_ftp.authed = false; g_ftp.gotUser = false; g_ftp.gotRnfr = false;
            g_ftp.atVirtualRoot = false; g_ftp.listPending = false;
            g_ftp.ctrlHalfClosed = false; g_ftp.retrPending = false; g_ftp.storPending = false;
            g_ftp.xferType = XFER_NONE; g_ftp.recvLen = 0;
            g_ftp.retrBufLen = 0; g_ftp.retrBufOff = 0;
            g_ftp.listBufLen = 0; g_ftp.listBufOff = 0;
            g_ftp.sendLen = 0; g_ftp.sendOff = 0;
            g_ftp.state = FTP_LISTEN;
            return;
        }
    }

    // ---- Drain secondary connection attempts --------------------------------
    // FileZilla opens a second ctrl connection for transfers while keeping the
    // browsing connection open. Our server is single-session, so we accept and
    // immediately reject any connection that arrives while already connected.
    // This is critical: leaving it stuck in the listen backlog causes FileZilla
    // to wait until its own connection timeout fires, which it reports as a
    // disconnect. Sending 421 immediately tells FileZilla to reuse the existing
    // session instead of waiting on the stalled second connection.
    if ((g_ftp.state == FTP_CONNECTED || g_ftp.state == FTP_TRANSFER) &&
        g_ftp.listenSock != INVALID_SOCKET)
    {
        // Drain ALL pending secondary connections this tick (FileZilla may open
        // several at once). The accepted socket INHERITS the listen socket's
        // non-blocking mode, so a plain send() can return WSAEWOULDBLOCK and the
        // 421 never goes out before we close -- FileZilla then sees an abortive
        // close and reports a failed session instead of reusing the existing one.
        // Force the socket blocking and linger-on-close so the reply is actually
        // delivered, then FileZilla falls back to the single live session.
        const char* k_busy = "421 Only one session permitted.\r\n";
        int len = 0; while (k_busy[len]) len++;
        for (;;)
        {
            SOCKADDR_IN ca; int caLen = sizeof(ca);
            SOCKET cs = accept(g_ftp.listenSock, (SOCKADDR*)&ca, &caLen);
            struct linger lg;
            u_long blk = 0;
            if (cs == INVALID_SOCKET) break;
            ioctlsocket(cs, FIONBIO, &blk);                 /* -> blocking          */
            lg.l_onoff = 1; lg.l_linger = 1;                /* flush on close       */
            setsockopt(cs, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof(lg));
            send(cs, k_busy, len, 0);
            closesocket(cs);
        }
    }

    // ---- Accept new control connection ------------------------------------
    if (g_ftp.state == FTP_LISTEN && g_ftp.listenSock != INVALID_SOCKET)
    {
        SOCKADDR_IN ca; int caLen = sizeof(ca);
        SOCKET cs = accept(g_ftp.listenSock, (SOCKADDR*)&ca, &caLen);
        if (cs != INVALID_SOCKET)
        {
            u_long nb = 1; ioctlsocket(cs, FIONBIO, &nb);
            g_ftp.ctrlSock = cs;
            g_ftp.authed = false;
            g_ftp.gotUser = false;
            g_ftp.gotRnfr = false;
            g_ftp.atVirtualRoot = false;
            g_ftp.ctrlHalfClosed = false;
            g_ftp.xferType = XFER_NONE;
            g_ftp.recvLen = 0;
            g_ftp.restOffset = 0;
            g_ftp.alloSize = 0;
            g_ftp.lastActivityMs = GetTickCount();
            g_ftp.state = FTP_CONNECTED;
            // Multi-line 220 banner — FTP spec: "NNN-" for continuation, "NNN " for final line.
            // FlashFXP and FileZilla display the continuation lines in their message/log panel.
            FtpSendStr(g_ftp.ctrlSock, "220-DarkDash FTP Server\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220-Original Xbox Dashboard\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220-Team Resurgent  /  Darkone83\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220-\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220-Mode : Passive (PASV) only\r\n");
            FtpSendStr(g_ftp.ctrlSock, "220 Ready.\r\n");
            // Fall through — don't return. The send-drain at the top of the
            // next-tick pass needs the banner to be flushed promptly. Returning
            // here would defer it one full frame on every new connection.
        }
        else
        {
            // No pending connection this tick — nothing else to do in LISTEN state.
            return;
        }
    }

    // ---- Accept passive data connection ----------------------------------
    // Try to accept whenever dataListen is open and we don't have dataSock yet.
    if (g_ftp.dataSock == INVALID_SOCKET &&
        g_ftp.dataListen != INVALID_SOCKET &&
        (g_ftp.state == FTP_CONNECTED || g_ftp.state == FTP_TRANSFER))
    {
        SOCKADDR_IN da; int daLen = sizeof(da);
        SOCKET ds = accept(g_ftp.dataListen, (SOCKADDR*)&da, &daLen);
        if (ds != INVALID_SOCKET)
        {
            u_long nb = 1; ioctlsocket(ds, FIONBIO, &nb);

            // Expand TCP send buffer to 256KB — Xbox default is ~8KB which causes
            // constant WSAEWOULDBLOCK even with the spin loop, capping throughput.
            // 256KB gives the kernel enough runway to keep the wire saturated.
            int sndbuf = 256 * 1024;
            setsockopt(ds, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

            g_ftp.dataSock = ds;

            // If a LIST was waiting for a data connection, execute it now
            if (g_ftp.listPending)
                FtpDoListing();

            // If a RETR or STOR was waiting, send the deferred 150 now
            if (g_ftp.retrPending)
            {
                FtpReply(150, "Opening data connection.");
                g_ftp.retrPending = false;
                g_ftp.state = FTP_TRANSFER;
            }
            else if (g_ftp.storPending)
            {
                FtpReply(150, "Opening data connection.");
                g_ftp.storPending = false;
                g_ftp.state = FTP_TRANSFER;
            }
        }
    }

    // Fallback: listPending but dataSock already accepted (fast client connected
    // to data port before LIST command arrived — dispatch was handled inline in
    // FtpHandleCommand, but catch any edge case where it was missed).
    if (g_ftp.listPending &&
        g_ftp.dataSock != INVALID_SOCKET &&
        g_ftp.xferType == XFER_NONE)
    {
        FtpDoListing();
    }

    // Deferred RETR/STOR 150: dataSock just became available — send 150 now and
    // promote to FTP_TRANSFER so the data pump engages this tick.
    if (g_ftp.retrPending && g_ftp.dataSock != INVALID_SOCKET)
    {
        FtpReply(150, "Opening data connection.");
        g_ftp.retrPending = false;
        g_ftp.state = FTP_TRANSFER;
    }
    if (g_ftp.storPending && g_ftp.dataSock != INVALID_SOCKET)
    {
        FtpReply(150, "Opening data connection.");
        g_ftp.storPending = false;
        g_ftp.state = FTP_TRANSFER;
    }

    // ---- Control socket: receive and command dispatch --------------------
    if ((g_ftp.state == FTP_CONNECTED || g_ftp.state == FTP_TRANSFER) &&
        g_ftp.ctrlSock != INVALID_SOCKET)
    {
        // Always try to recv — don't gate on send buffer fullness.
        // A 8KB send buffer gives enough headroom; pausing recv causes
        // FileZilla to time out waiting for command acknowledgement.
        //
        // recv == 0 is a TCP half-close: the peer (FileZilla) shut down its write
        // side after its last command but still expects our pending replies (e.g.
        // 226 Transfer complete) on the ctrl socket.  Set ctrlHalfClosed and stop
        // reading; the send-drain at the top of tick will flush the buffer, and we
        // tear down only once it is empty.  Treating half-close as a hard error was
        // the root cause of random FileZilla disconnects before 226 was delivered.
        {
            int space = (int)sizeof(g_ftp.recvBuf) - g_ftp.recvLen - 1;
            if (space > 0 && !g_ftp.ctrlHalfClosed)
            {
                int n = recv(g_ftp.ctrlSock,
                    g_ftp.recvBuf + g_ftp.recvLen, space, 0);
                if (n > 0) { g_ftp.recvLen += n; g_ftp.lastActivityMs = GetTickCount(); }
                else if (n == 0)
                {
                    // Graceful half-close — stop receiving, keep sending
                    g_ftp.ctrlHalfClosed = true;
                }
                else if (WSAGetLastError() != WSAEWOULDBLOCK)
                    goto ctrl_disconnect;  // hard socket error
            }
            else
            {
                // recvBuf full — discard up to next newline, keep remainder
                int nl = -1;
                for (int i = 0; i < g_ftp.recvLen; ++i)
                    if (g_ftp.recvBuf[i] == '\n') { nl = i; break; }
                if (nl >= 0)
                {
                    int remaining = g_ftp.recvLen - nl - 1;
                    for (int i = 0; i < remaining; ++i)
                        g_ftp.recvBuf[i] = g_ftp.recvBuf[nl + 1 + i];
                    g_ftp.recvLen = remaining;
                }
                else g_ftp.recvLen = 0;
            }
        }

        // Parse and dispatch all complete commands already in recvBuf.
        // This runs regardless of send buffer state — commands buffered before
        // the gate closed still need to be processed.
        {
            g_ftp.recvBuf[g_ftp.recvLen] = '\0';
            char* buf = g_ftp.recvBuf;
            while (true)
            {
                int pos = -1;
                for (int i = 0; i < g_ftp.recvLen; ++i)
                    if (buf[i] == '\n') { pos = i; break; }
                if (pos < 0) break;

                if (pos > 0 && buf[pos - 1] == '\r') buf[pos - 1] = '\0';
                buf[pos] = '\0';

                FtpHandleCommand(buf);

                int remaining = g_ftp.recvLen - pos - 1;
                for (int i = 0; i < remaining; ++i) buf[i] = buf[pos + 1 + i];
                g_ftp.recvLen = remaining;
                buf[remaining] = '\0';
            }
        }
        // Half-close teardown: peer shut write side; once our send buffer is fully
        // drained (all replies delivered) we can close the session cleanly.
        // We do NOT dispatch any further commands once ctrlHalfClosed is set.
        if (g_ftp.ctrlHalfClosed && g_ftp.sendLen == g_ftp.sendOff)
            goto ctrl_disconnect;

        goto skip_ctrl_disconnect;

    ctrl_disconnect:
        {
            // Client disconnected or hard socket error — full session teardown
            FtpStorCloseFile(false);
            if (g_ftp.dataSock != INVALID_SOCKET) { closesocket(g_ftp.dataSock);   g_ftp.dataSock = INVALID_SOCKET; }
            if (g_ftp.dataListen != INVALID_SOCKET) { closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET; }
            closesocket(g_ftp.ctrlSock); g_ftp.ctrlSock = INVALID_SOCKET;
            g_ftp.authed = false;
            g_ftp.gotUser = false;
            g_ftp.gotRnfr = false;
            g_ftp.atVirtualRoot = false;
            g_ftp.ctrlHalfClosed = false;
            g_ftp.listPending = false;
            g_ftp.retrPending = false;
            g_ftp.storPending = false;
            g_ftp.xferType = XFER_NONE;
            g_ftp.recvLen = 0;
            g_ftp.retrBufLen = 0;
            g_ftp.retrBufOff = 0;
            g_ftp.listBufLen = 0;
            g_ftp.listBufOff = 0;
            g_ftp.sendLen = 0;
            g_ftp.sendOff = 0;
            g_ftp.restOffset = 0;
            g_ftp.alloSize = 0;
            g_ftp.state = FTP_LISTEN;
            return;
        }
    skip_ctrl_disconnect:;
    }

    // ---- Data transfer ---------------------------------------------------
    if (g_ftp.state == FTP_TRANSFER && g_ftp.dataSock != INVALID_SOCKET)
    {
        // STOR uses the persistent 128 KiB staging buffer declared above.

        if (g_ftp.xferType == XFER_LIST)
        {
            // Drain listBuf into dataSock non-blockingly, same pattern as RETR
            if (g_ftp.listBufOff < g_ftp.listBufLen)
            {
                int toSend = g_ftp.listBufLen - g_ftp.listBufOff;
                int sent = send(g_ftp.dataSock,
                    g_ftp.listBuf + g_ftp.listBufOff, toSend, 0);
                if (sent > 0)
                {
                    g_ftp.listBufOff += sent;
                }
                else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
                {
                    // Data socket died mid-listing
                    closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                    FtpReply(426, "Connection closed; transfer aborted.");
                    g_ftp.xferType = XFER_NONE;
                    g_ftp.state = FTP_CONNECTED;
                    return;
                }
                // WSAEWOULDBLOCK — just wait for next tick
            }
            else
            {
                // All listing data sent — close data socket and reply 226
                closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                if (g_ftp.dataListen != INVALID_SOCKET)
                {
                    closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET;
                }
                FtpReply(226, "Transfer complete.");
                g_ftp.xferType = XFER_NONE;
                g_ftp.state = FTP_CONNECTED;
            }
        }
        else if (g_ftp.xferType == XFER_RETR)
        {
            // Time-bounded pump: keep reading + sending for up to 30ms per tick.
            // On WSAEWOULDBLOCK we spin within the time window rather than bailing
            // for a full frame — this is the key to hitting max throughput since
            // the TCP send buffer drains in microseconds and we'd waste the rest
            // of the budget if we broke out immediately.
            DWORD retrStart = GetTickCount();
            while (GetTickCount() - retrStart < 30)
            {
                // Drain any unsent remainder from the previous read first
                if (g_ftp.retrBufOff < g_ftp.retrBufLen)
                {
                    int toSend = g_ftp.retrBufLen - g_ftp.retrBufOff;
                    int sent = send(g_ftp.dataSock,
                        g_ftp.retrBuf + g_ftp.retrBufOff, toSend, 0);
                    if (sent > 0)
                    {
                        g_ftp.retrBufOff += sent;
                        g_ftp.xferDone += (DWORD)sent;
                        // Keep looping — may have drained fully, can read next chunk
                        continue;
                    }
                    else if (sent < 0)
                    {
                        if (WSAGetLastError() == WSAEWOULDBLOCK)
                            break;  // TCP send buffer full — yield to next tick
                        // Hard error
                        closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                        CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
                        FtpReply(426, "Transfer aborted.");
                        g_ftp.xferType = XFER_NONE;
                        g_ftp.state = FTP_CONNECTED;
                        goto retr_done;
                    }
                    continue;
                }

                // Buffer fully drained — read the next chunk from disk
                g_ftp.retrBufLen = 0;
                g_ftp.retrBufOff = 0;
                DWORD bytesRead = 0;
                if (ReadFile(g_ftp.xferFile, g_ftp.retrBuf, sizeof(g_ftp.retrBuf),
                    &bytesRead, NULL) && bytesRead > 0)
                {
                    g_ftp.retrBufLen = (int)bytesRead;
                    // Fall through to drain path at top of loop next iteration
                }
                else
                {
                    // EOF or read error — transfer complete
                    closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                    if (g_ftp.dataListen != INVALID_SOCKET)
                    {
                        closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET;
                    }
                    CloseHandle(g_ftp.xferFile); g_ftp.xferFile = INVALID_HANDLE_VALUE;
                    FtpReply(226, "Transfer complete.");
                    g_ftp.xferType = XFER_NONE;
                    g_ftp.state = FTP_CONNECTED;
                    goto retr_done;
                }
            }
        retr_done:;
        }
        else if (g_ftp.xferType == XFER_STOR)
        {
            // Preserve partial blocks across service ticks; the dedicated
            // worker can run between frames without waiting for D3D Present.
            // The 15 ms budget is retained to keep stop/restart responsive.
            DWORD storStart = GetTickCount();
            while (GetTickCount() - storStart < 15)
            {
#if DD_FTP_BATCHED_STOR
                int n = recv(g_ftp.dataSock, s_storBuf + s_storBuffered,
                    (int)sizeof(s_storBuf) - s_storBuffered, 0);
#else
                int n = recv(g_ftp.dataSock, s_storBuf,
                    (int)sizeof(s_storBuf), 0);
#endif
                if (n > 0)
                {
#if DD_FTP_BATCHED_STOR
                    s_storBuffered += n;
                    if (s_storBuffered < (int)sizeof(s_storBuf))
                        continue;  // accumulate into the next frame if needed
                    if (!FtpStorCommitBuffered())
#else
                    DWORD written = 0;
#if DD_FTP_PREALLOC_STOR
                    bool spaceReady = FtpStorReserveForWrite((DWORD)n);
#else
                    bool spaceReady = true;
#endif
                    if (!spaceReady ||
                        !WriteFile(g_ftp.xferFile, s_storBuf, (DWORD)n, &written, NULL) ||
                        written != (DWORD)n)
#endif
                    {
                        s_storBuffered = 0;
                        closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                        FtpStorCloseFile(false);
                        FtpReply(452, "Write error - disk full?");
                        g_ftp.xferType = XFER_NONE;
                        g_ftp.state = FTP_CONNECTED;
                        return;
                    }
#if !DD_FTP_BATCHED_STOR
                    g_ftp.xferDone += written;
#endif
                    // continue draining while socket has data and time remains
                }
                else if (n == 0)
                {
                    // EOF is the only time we commit the incomplete final block.
                    // Do not report 226 until the buffered bytes are on disk.
#if DD_FTP_BATCHED_STOR
                    if (s_storBuffered && !FtpStorCommitBuffered())
                    {
                        s_storBuffered = 0;
                        closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                        FtpStorCloseFile(false);
                        FtpReply(452, "Write error - disk full?");
                        g_ftp.xferType = XFER_NONE;
                        g_ftp.state = FTP_CONNECTED;
                        return;
                    }
#endif
                    bool fileFinished = FtpStorCloseFile(true);
                    closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                    if (g_ftp.dataListen != INVALID_SOCKET)
                    {
                        closesocket(g_ftp.dataListen); g_ftp.dataListen = INVALID_SOCKET;
                    }
                    FtpReply(fileFinished ? 226 : 452,
                        fileFinished ? "Transfer complete." : "File finalization failed.");
                    g_ftp.xferType = XFER_NONE;
                    g_ftp.state = FTP_CONNECTED;
                    goto stor_done;
                }
                else
                {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK)
                        break; // leave partial block intact; retry next frame
                    // Client aborted; discard uncommitted bytes so they cannot
                    // leak into the next upload (REST sees the on-disk offset).
                    s_storBuffered = 0;
                    closesocket(g_ftp.dataSock); g_ftp.dataSock = INVALID_SOCKET;
                    FtpStorCloseFile(false);
                    FtpReply(426, "Transfer aborted.");
                    g_ftp.xferType = XFER_NONE;
                    g_ftp.state = FTP_CONNECTED;
                    goto stor_done;
                }
            }
        stor_done:;
        }
    }
}

/* ============================================================================
   DarkDash service wrappers and opt-in FTP worker.

   Important lifecycle contract: dd_net.cpp calls FtpServ_Stop() BEFORE
   WSACleanup/XNetCleanup during Net_Restart(). We MUST join the worker before
   closing a socket, file handle, or resetting the network stack. Only the
   worker may invoke FtpServ_{Start,Tick,StopEngine} while threading is on.
   ============================================================================ */

static int s_ftpWanted = 0;

#if DD_FTP_WORKER_THREAD
/* The main thread creates and joins this handle; only the worker touches the
   FTP engine. Snapshots are 32-bit, aligned and published via InterlockedExchange
   so UI/status readers never inspect partially updated engine data. */
static HANDLE s_workerHandle = NULL;
static LONG s_workerStop = 0;
static LONG s_ftpStatus = FTP_OFF;
static LONG s_ftpDone = 0;
static LONG s_ftpTotal = 0;
static DWORD s_lastWorkerAttemptMs = 0;
static char s_workerIP[20];

static void FtpWorkerPublish()
{
    InterlockedExchange(&s_ftpDone, (LONG)g_ftp.xferDone);
    InterlockedExchange(&s_ftpTotal, (LONG)g_ftp.xferTotal);
    InterlockedExchange(&s_ftpStatus, (LONG)g_ftp.state);
}

static DWORD WINAPI FtpWorkerMain(LPVOID)
{
#if DD_FTP_USE_FILE_CACHE
    /* Global Xbox file cache, not a private transfer buffer. Leave disabled
       until on-hardware RAM and throughput profiling proves it useful. */
    XSetFileCacheSize(4 * 1024 * 1024);
#endif
    FtpServ_Start(s_workerIP, true);
    FtpWorkerPublish();
    if (g_ftp.state != FTP_OFF)
    {
        while (InterlockedCompareExchange(&s_workerStop, 0, 0) == 0)
        {
            FtpServ_Tick();
            FtpWorkerPublish();
            /* Do not busy-spin on nonblocking WSAEWOULDBLOCK. Normal
               priority plus Sleep gives the UI time on the single CPU. */
            Sleep(g_ftp.state == FTP_LISTEN ? 5 : 1);
        }
    }
    FtpServ_StopEngine();
    FtpWorkerPublish();
    return 0;
}

static void FtpWorkerStop()
{
    if (s_workerHandle != NULL)
    {
        InterlockedExchange(&s_workerStop, 1);
        /* The worker owns ALL FTP sockets/file handles. Never tear down the
           Xbox network stack or close sockets until it has finished. */
        WaitForSingleObject(s_workerHandle, INFINITE);
        CloseHandle(s_workerHandle);
        s_workerHandle = NULL;
    }
    InterlockedExchange(&s_ftpStatus, FTP_OFF);
    InterlockedExchange(&s_ftpDone, 0);
    InterlockedExchange(&s_ftpTotal, 0);
    s_lastWorkerAttemptMs = 0;
}

static void FtpWorkerStart(const char* ip)
{
    if (s_workerHandle != NULL || !ip || !*ip) return;
    StrCopy(s_workerIP, sizeof(s_workerIP), ip);
    InterlockedExchange(&s_workerStop, 0);
    s_lastWorkerAttemptMs = GetTickCount();
    s_workerHandle = CreateThread(NULL, 0, FtpWorkerMain, NULL, 0, NULL);
    /* Normal priority by default; do not steal all CPU time from renderer.
       On failure, Ftp_Tick retries after a bounded one-second interval. */
}
#endif

/* Public stop entry point is also called directly by dd_net.cpp during
   network restart. The original, synchronous behavior is preserved when the
   threaded experiment is disabled. Keep the user's desired enabled state. */
void FtpServ_Stop(void)
{
#if DD_FTP_WORKER_THREAD
    FtpWorkerStop();
#else
    FtpServ_StopEngine();
#endif
}

void Ftp_Init(void)
{
    g_ftp.state = FTP_OFF;
    s_ftpWanted = 0;
#if DD_FTP_WORKER_THREAD
    s_workerHandle = NULL;
    s_lastWorkerAttemptMs = 0;
    InterlockedExchange(&s_workerStop, 0);
    InterlockedExchange(&s_ftpStatus, FTP_OFF);
    InterlockedExchange(&s_ftpDone, 0);
    InterlockedExchange(&s_ftpTotal, 0);
#endif
}

void Ftp_Want(int want)
{
    s_ftpWanted = want ? 1 : 0;
    if (!s_ftpWanted) FtpServ_Stop();
}

void Ftp_Start(const char* ipStr, int ipOK)
{
    s_ftpWanted = 1;
#if DD_FTP_WORKER_THREAD
    FtpServ_Stop();
    if (ipOK) FtpWorkerStart(ipStr);
#else
    FtpServ_Start(ipStr, ipOK != 0);
#endif
}

void Ftp_Stop(void)
{
    s_ftpWanted = 0;
    FtpServ_Stop();
}

void Ftp_Tick(void)
{
#if DD_FTP_WORKER_THREAD
    /* The main thread only manages lifecycle, never data sockets or files. */
    if (s_workerHandle != NULL &&
        WaitForSingleObject(s_workerHandle, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(s_workerHandle);
        s_workerHandle = NULL;
    }
    if (!s_ftpWanted)
    {
        if (s_workerHandle != NULL) FtpServ_Stop();
        return;
    }
    if (!Net_IsUp())
    {
        if (s_workerHandle != NULL) FtpServ_Stop();
        return;
    }
    if (s_workerHandle == NULL)
    {
        DWORD now = GetTickCount();
        if (s_lastWorkerAttemptMs == 0 ||
            now - s_lastWorkerAttemptMs >= 1000)
            FtpWorkerStart(Net_Ip());
    }
#else
    /* Legacy frame-polled control path for exact A/B comparison. */
    if (s_ftpWanted && g_ftp.state == FTP_OFF && Net_IsUp())
        FtpServ_Start(Net_Ip(), true);
    if (g_ftp.state != FTP_OFF) FtpServ_Tick();
#endif
}

int Ftp_IsRunning(void)
{
#if DD_FTP_WORKER_THREAD
    return InterlockedCompareExchange(&s_ftpStatus, 0, 0) != FTP_OFF;
#else
    return g_ftp.state != FTP_OFF;
#endif
}
int Ftp_Wanted(void) { return s_ftpWanted; }
int Ftp_Status(void)
{
#if DD_FTP_WORKER_THREAD
    return (int)InterlockedCompareExchange(&s_ftpStatus, 0, 0);
#else
    return (int)g_ftp.state;
#endif
}
void Ftp_Progress(unsigned long* done, unsigned long* total)
{
#if DD_FTP_WORKER_THREAD
    if (done)  *done = (unsigned long)InterlockedCompareExchange(&s_ftpDone, 0, 0);
    if (total) *total = (unsigned long)InterlockedCompareExchange(&s_ftpTotal, 0, 0);
#else
    if (done)  *done = g_ftp.xferDone;
    if (total) *total = g_ftp.xferTotal;
#endif
}
