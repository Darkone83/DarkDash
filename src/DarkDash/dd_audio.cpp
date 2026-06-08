/*---------------------------------------------------------------------------
    dd_audio.cpp -- minimp3 decode -> DirectSound.
    SFX: small one-shot files, fully decoded into static buffers.
    Music: bg.mp3 STREAMED through a 256KB ring buffer + worker thread
           (ScorchedXB pattern) -- never full-decoded. A full 3-4 min
           track is ~40MB of PCM, which will not fit in one Xbox DS buffer.
    Defines the minimp3 implementation in this single translation unit.
    Links dsound.lib.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <dsound.h>
#include <stdlib.h>
#include <string.h>

/* Xbox CPU is a Pentium III (SSE1 only); minimp3's SIMD path is SSE2, and on
   a modern MSVC its <intrin.h>/<immintrin.h> include pulls intrin0.inl.h,
   which clashes with xtl.h's extern "C" _Interlocked* decls (C2733). The
   scalar path is both correct for this CPU and conflict-free. */
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "dd_audio.h"
#include "dd_fileops.h"  /* Fileops_LoadFile: generic file -> buffer */

#define AUDIO_SND_ROOT   "D:\\audio\\snd"
#define AUDIO_MUSIC_ROOT "D:\\audio\\music"
#define AUDIO_PATH_MAX   272

   /* Streaming ring: two halves the worker refills behind the play cursor. */
#define AUDIO_STREAM_BUFSIZE (256 * 1024)
#define AUDIO_STREAM_HALF    (AUDIO_STREAM_BUFSIZE / 2)

static LPDIRECTSOUND        s_ds = NULL;
static LPDIRECTSOUNDBUFFER  s_sfx[SFX_COUNT] = { 0 };

static int s_dbgRead = 0;       /* files read off disk               */
static int s_dbgDecoded = 0;    /* files minimp3 decoded to PCM     */

/* SFX files in enum order */
static const char* k_sfx_files[SFX_COUNT] = {
    "button-up.mp3",    /* SFX_NAV_UP   */
    "button-down.mp3",  /* SFX_NAV_DOWN */
    "button-a.mp3",     /* SFX_SELECT   */
    "button-x.mp3",     /* SFX_ALT      */
    "button-other.mp3"  /* SFX_BACK     */
};

/* =========================================================================
   SFX: full decode -> static buffer (small files only)
========================================================================= */

static int decode_mp3_file(const char* path, mp3d_sample_t** pcmOut,
    int* bytesOut, int* chOut, int* hzOut) {
    unsigned char* data = NULL;
    size_t size = 0;
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    mp3d_sample_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3d_sample_t* out = NULL;
    int outSamples = 0, outCap = 0;
    const unsigned char* p;
    int rem, ch = 0, hz = 0;

    *pcmOut = NULL; *bytesOut = 0; *chOut = 0; *hzOut = 0;

    if (Fileops_LoadFile(&data, &size, path) != 0 || !data || size == 0) {
        if (data) free(data);
        return 0;
    }
    s_dbgRead++;   /* file bytes were read off disk OK */

    mp3dec_init(&dec);
    p = data; rem = (int)size;

    while (rem > 0) {
        int samples = mp3dec_decode_frame(&dec, p, rem, frame, &info);
        if (info.frame_bytes <= 0) break;          /* no more frames */
        if (samples > 0) {
            int n = samples * info.channels;       /* int16 count this frame */
            if (outSamples + n > outCap) {
                int newCap = outCap ? outCap * 2 : 65536;
                mp3d_sample_t* tmp;
                while (newCap < outSamples + n) newCap *= 2;
                tmp = (mp3d_sample_t*)realloc(out, (size_t)newCap * sizeof(mp3d_sample_t));
                if (!tmp) { free(out); free(data); return 0; }
                out = tmp; outCap = newCap;
            }
            memcpy(out + outSamples, frame, (size_t)n * sizeof(mp3d_sample_t));
            outSamples += n;
            ch = info.channels; hz = info.hz;
        }
        p += info.frame_bytes; rem -= info.frame_bytes;
    }
    free(data);

    if (outSamples == 0 || ch == 0 || hz == 0) { if (out) free(out); return 0; }
    s_dbgDecoded++;   /* minimp3 produced PCM OK */
    *pcmOut = out;
    *bytesOut = outSamples * (int)sizeof(mp3d_sample_t);
    *chOut = ch;
    *hzOut = hz;
    return 1;
}

static LPDIRECTSOUNDBUFFER make_buffer(const mp3d_sample_t* pcm, int bytes,
    int ch, int hz) {
    WAVEFORMATEX wfx;
    DSBUFFERDESC dsbd;
    LPDIRECTSOUNDBUFFER buf = NULL;
    void* p1 = NULL, * p2 = NULL;
    DWORD b1 = 0, b2 = 0;
    HRESULT hr;

    if (!s_ds || !pcm || bytes <= 0) return NULL;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)ch;
    wfx.nSamplesPerSec = (DWORD)hz;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(ch * 2);
    wfx.nAvgBytesPerSec = (DWORD)(hz * ch * 2);
    wfx.cbSize = 0;

    ZeroMemory(&dsbd, sizeof(dsbd));
    dsbd.dwSize = sizeof(dsbd);
    dsbd.dwFlags = DSBCAPS_CTRLVOLUME;
    dsbd.dwBufferBytes = (DWORD)bytes;
    dsbd.lpwfxFormat = &wfx;

    hr = s_ds->CreateSoundBuffer(&dsbd, &buf, NULL);
    if (FAILED(hr) || !buf) return NULL;

    hr = buf->Lock(0, (DWORD)bytes, &p1, &b1, &p2, &b2, 0);
    if (FAILED(hr)) { buf->Release(); return NULL; }
    if (p1 && b1) memcpy(p1, pcm, b1);
    if (p2 && b2) memcpy(p2, (const char*)pcm + b1, b2);
    buf->Unlock(p1, b1, p2, b2);

    return buf;
}

static LPDIRECTSOUNDBUFFER load_sound(const char* root, const char* file) {
    char path[AUDIO_PATH_MAX];
    mp3d_sample_t* pcm = NULL;
    int bytes = 0, ch = 0, hz = 0;
    LPDIRECTSOUNDBUFFER buf;

    strncpy(path, root, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    strncat(path, "\\", sizeof(path) - strlen(path) - 1);
    strncat(path, file, sizeof(path) - strlen(path) - 1);

    if (!decode_mp3_file(path, &pcm, &bytes, &ch, &hz)) return NULL;
    buf = make_buffer(pcm, bytes, ch, hz);
    free(pcm);
    return buf;
}

/* =========================================================================
   Music: streamed (decode-on-demand into a small ring buffer)
========================================================================= */

static LPDIRECTSOUNDBUFFER s_music = NULL;   /* the 256KB streaming ring */
static unsigned char* s_musicData = NULL;  /* whole mp3 file in RAM   */
static DWORD               s_musicSize = 0;
static DWORD               s_musicPos = 0;     /* read cursor into mp3    */
static mp3dec_t            s_musicDec;
static HANDLE              s_musicThread = NULL;
static LONG                s_musicStop = 0;
static LONG                s_musicEOF = 0;
static int                 s_musicLoop = 1;
static DWORD               s_musicEofTick = 0;   /* GetTickCount when a non-looping track hit decoder EOF */
static DWORD               s_musicDrainMs = 1500;/* ms for the already-buffered tail to finish playing    */
#define SEG_BYTES  2048                          /* ~512 stereo samples per level cell */
#define NSEG       (AUDIO_STREAM_BUFSIZE / SEG_BYTES)
static LONG  s_segLow[NSEG];                      /* per-buffer-segment bass level  (fill writes) */
static LONG  s_segHigh[NSEG];                     /* per-buffer-segment treble level (fill writes) */
static int   s_lpState = 0;                       /* one-pole lowpass state (fill thread)         */
static LONG  s_levelLow = 0;                      /* smoothed bass output  (main thread)          */
static LONG  s_levelHigh = 0;                     /* smoothed treble output (main thread)         */
static CRITICAL_SECTION    s_musicCS;
static int                 s_csReady = 0;

/* carry-over for a frame that straddles a half boundary (avoids pops) */
static short s_pcmCarry[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
static DWORD s_carryBytes = 0;

static void FillHalf(int nHalf) {
    void* pData1 = NULL;
    DWORD  dwLen1 = 0;
    BYTE* pDst;
    DWORD  dwFilled;
    short  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];

    if (!s_music) return;
    if (FAILED(s_music->Lock((DWORD)nHalf * AUDIO_STREAM_HALF, AUDIO_STREAM_HALF,
        &pData1, &dwLen1, NULL, NULL, 0))) return;

    pDst = (BYTE*)pData1;
    dwFilled = 0;

    EnterCriticalSection(&s_musicCS);

    /* drain any carry-over from the previous fill first */
    if (s_carryBytes > 0) {
        DWORD canWrite = s_carryBytes < dwLen1 ? s_carryBytes : dwLen1;
        memcpy(pDst, s_pcmCarry, canWrite);
        dwFilled = canWrite;
        if (canWrite < s_carryBytes)
            memmove(s_pcmCarry, (BYTE*)s_pcmCarry + canWrite, s_carryBytes - canWrite);
        s_carryBytes -= canWrite;
    }

    while (dwFilled < dwLen1) {
        mp3dec_frame_info_t info;
        int   nSamples;
        DWORD dwFrameBytes, dwCanWrite;

        if (s_musicPos >= s_musicSize) {
            if (!s_musicLoop) {
                InterlockedExchange(&s_musicEOF, 1);
                if (s_musicEofTick == 0) s_musicEofTick = GetTickCount();  /* tail starts draining now */
                memset(pDst + dwFilled, 0, dwLen1 - dwFilled);
                dwFilled = dwLen1;
                break;
            }
            s_musicPos = 0;        /* loop back to the start */
            s_carryBytes = 0;
        }

        nSamples = mp3dec_decode_frame(&s_musicDec, s_musicData + s_musicPos,
            (int)(s_musicSize - s_musicPos), pcm, &info);
        if (info.frame_bytes > 0) s_musicPos += (DWORD)info.frame_bytes;
        else                      s_musicPos = 0;
        if (nSamples <= 0) continue;

        dwFrameBytes = (DWORD)(nSamples * info.channels * (int)sizeof(short));
        dwCanWrite = dwLen1 - dwFilled;

        if (dwFrameBytes <= dwCanWrite) {
            memcpy(pDst + dwFilled, pcm, dwFrameBytes);
            dwFilled += dwFrameBytes;
        }
        else {
            memcpy(pDst + dwFilled, pcm, dwCanWrite);
            dwFilled = dwLen1;
            s_carryBytes = dwFrameBytes - dwCanWrite;
            memcpy(s_pcmCarry, (BYTE*)pcm + dwCanWrite, s_carryBytes);
        }
    }

    LeaveCriticalSection(&s_musicCS);

    /* per-segment band levels for the audio just written (lock-free: the main
       thread reads these by play-cursor position -- no second buffer Lock). A
       one-pole lowpass splits bass (lp) from treble (residual). */
    {
        int seg0 = (int)((nHalf * AUDIO_STREAM_HALF) / SEG_BYTES);
        int nseg = AUDIO_STREAM_HALF / SEG_BYTES;
        int sps = SEG_BYTES / 2, sg, k;
        short* sp = (short*)pData1;
        int lp = s_lpState;
        for (sg = 0; sg < nseg; sg++) {
            unsigned long bsum = 0, tsum = 0;
            for (k = 0; k < sps; k++) {
                int v = sp[sg * sps + k]; lp += (v - lp) >> 4;     /* ~440Hz lowpass */
                { int bb = lp, hh = v - lp; if (bb < 0) bb = -bb; if (hh < 0) hh = -hh; bsum += (unsigned long)bb; tsum += (unsigned long)hh; }
            }
            {
                LONG lo = (LONG)(bsum / sps) * 3; if (lo > 32767) lo = 32767;
                LONG hi = (LONG)(tsum / sps) * 6; if (hi > 32767) hi = 32767;
                s_segLow[seg0 + sg] = lo; s_segHigh[seg0 + sg] = hi;
            }
        }
        s_lpState = lp;
    }

    s_music->Unlock(pData1, dwLen1, NULL, 0);
}

static DWORD WINAPI MusicThreadProc(LPVOID pParam) {
    /* StartMusic primes BOTH halves before Play() and the cursor starts in
       half 0, so the behind-cursor half (1) is already fresh -- start with it
       marked filled so we don't immediately overwrite unplayed primed audio. */
    int lastFilled = 1;
    (void)pParam;
    /* DirectSoundDoWork() is owned by the main thread (Audio_Update), so the
       mixer is serviced even before music starts and SFX always play.

       Streaming model: the buffer has two halves. Whichever half the PLAY
       cursor is currently in is "live"; the OTHER half is the one we must keep
       filled ahead of the cursor. Each tick we read the real play position,
       work out the half behind the cursor, and (re)fill it once per crossing.
       Tracking the cursor directly -- rather than a separate toggle that can
       drift out of sync after a frame hitch -- removes the window where the
       cursor could lap into a half still holding stale audio (the random pop). */
    while (!InterlockedCompareExchange(&s_musicStop, 0, 0)) {
        DWORD dwPlay = 0, dwWrite = 0;
        int   playHalf, fillHalf;

        if (!s_music) { lastFilled = -1; Sleep(4); continue; }
        /* After a non-looping track hits EOF we keep filling: FillHalf pads
           silence, so the genuine tail plays out once and everything after it is
           silence (no stale re-loop) until the shuffle pump starts the next track. */

        s_music->GetCurrentPosition(&dwPlay, &dwWrite);
        playHalf = (dwPlay < (DWORD)AUDIO_STREAM_HALF) ? 0 : 1;
        fillHalf = 1 - playHalf;     /* the half behind the cursor */

        /* fill the behind-cursor half once per crossing; re-arm when the
           cursor moves into the half we just filled. */
        if (fillHalf != lastFilled) {
            FillHalf(fillHalf);
            lastFilled = fillHalf;
        }
        Sleep(4);
    }
    return 0;
}

/* =========================================================================
   Public
========================================================================= */

int Audio_Init(void) {
    int i;
    HRESULT hr;

    if (s_ds) return 1;
    hr = DirectSoundCreate(NULL, &s_ds, NULL);
    if (FAILED(hr) || !s_ds) return 0;

    InitializeCriticalSection(&s_musicCS);
    s_csReady = 1;

    s_dbgRead = 0; s_dbgDecoded = 0;
    for (i = 0; i < SFX_COUNT; i++)
        s_sfx[i] = load_sound(AUDIO_SND_ROOT, k_sfx_files[i]);

    return 1;
}

void Audio_Update(void) {
    /* Required on Xbox: services the DS mixer / commits GP work each frame.
       Without it the play cursor stalls and there is no audio at all. */
    DirectSoundDoWork();
}

void Audio_PlaySfx(int id) {
    if (id < 0 || id >= SFX_COUNT) return;
    if (!s_sfx[id]) return;
    s_sfx[id]->SetCurrentPosition(0);   /* rewind, then play (ScorchedXB pattern) */
    s_sfx[id]->Play(0, 0, 0);
}

static LONG s_musicVolPct = 70;        /* 0..100, last requested percent  */
static LONG s_musicVolDb = -1200;     /* DirectSound attenuation, 0..-10000 */
static char s_musicFile[AUDIO_PATH_MAX] = { 0 };   /* full path; empty = default bg.mp3 */

/* Choose the bg track. Pass NULL/empty to use the built-in D:\audio\music\bg.mp3.
   Takes effect on the next Audio_StartMusic. */
void Audio_SetMusicPath(const char* fullPath) {
    if (!fullPath || !fullPath[0]) { s_musicFile[0] = 0; return; }
    strncpy(s_musicFile, fullPath, sizeof(s_musicFile) - 1);
    s_musicFile[sizeof(s_musicFile) - 1] = 0;
}

/* map 0..100% to a perceptual dB attenuation. 100% -> 0 (full), 0% -> silence.
   Uses a simple squared-ish curve so the low end isn't all crammed together. */
static LONG VolPctToDb(int pct) {
    if (pct <= 0)   return -10000;     /* DSBVOLUME_MIN: silent */
    if (pct >= 100) return 0;          /* DSBVOLUME_MAX: full    */
    /* -40 dB span feels natural for UI music; scale non-linearly */
    {
        long span = 4000;              /* 40 dB * 100 */
        long t = 100 - pct;         /* 0 at full, 100 at quiet */
        return -(span * t * t / 10000);/* quadratic taper, 0..-4000 */
    }
}

void Audio_SetMusicVolume(int pct) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_musicVolPct = pct;
    s_musicVolDb = VolPctToDb(pct);
    if (s_music) s_music->SetVolume(s_musicVolDb);   /* live if playing */
}

int Audio_GetMusicVolume(void) { return (int)s_musicVolPct; }

/* 1 only when a non-looping track has played to its natural end: the decoder hit
   EOF AND the buffered tail has had time to drain. Returns 0 while looping music
   plays and 0 when music is stopped (s_music NULL) -- so a deliberate stop (game
   launch / screensaver) never looks like a finished track. */
int Audio_MusicFinished(void) {
    if (!s_music) return 0;
    if (!InterlockedCompareExchange(&s_musicEOF, 1, 1)) return 0;
    if (s_musicEofTick == 0) return 0;
    return ((DWORD)(GetTickCount() - s_musicEofTick) >= s_musicDrainMs) ? 1 : 0;
}

/* Low (bass) and high (treble) loudness, each 0..255. Lock-free: reads the play
   cursor and looks up the per-segment level the fill thread precomputed, then
   applies fast-attack / slower-decay smoothing. lo/hi may be NULL. */
void Audio_MusicLevels(int* lo, int* hi) {
    DWORD play = 0, wr = 0; LONG tlo = 0, thi = 0, a, b;
    if (s_music && SUCCEEDED(s_music->GetCurrentPosition(&play, &wr))) {
        int seg = (int)(play / SEG_BYTES);
        if (seg < 0) seg = 0; else if (seg >= NSEG) seg = NSEG - 1;
        tlo = s_segLow[seg]; thi = s_segHigh[seg];
    }
    { LONG c = s_levelLow;  s_levelLow = (tlo > c) ? (c + (tlo - c) * 3 / 4) : (c + (tlo - c) / 3); }
    { LONG c = s_levelHigh; s_levelHigh = (thi > c) ? (c + (thi - c) * 7 / 8) : (c + (thi - c) / 2); }
    a = s_levelLow >> 7;  if (a > 255) a = 255;
    b = s_levelHigh >> 7; if (b > 255) b = 255;
    if (lo) *lo = (int)a;
    if (hi) *hi = (int)b;
}

/* Combined music loudness, 0..255 (kept for any single-band callers). */
int Audio_MusicLevel(void) {
    int lo = 0, hi = 0;
    Audio_MusicLevels(&lo, &hi);
    return (lo > hi) ? lo : hi;
}

void Audio_StartMusic(int loop) {
    char path[AUDIO_PATH_MAX];
    unsigned char* data = NULL;
    size_t size = 0;
    mp3dec_frame_info_t info;
    short probe[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    WAVEFORMATEX wfx;
    DSBUFFERDESC dsbd;

    if (!s_ds) return;
    Audio_StopMusic();   /* tear down any prior stream */

    if (s_musicFile[0]) {
        strncpy(path, s_musicFile, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
    }
    else {
        strncpy(path, AUDIO_MUSIC_ROOT, sizeof(path) - 1); path[sizeof(path) - 1] = 0;
        strncat(path, "\\bg.mp3", sizeof(path) - strlen(path) - 1);
    }

    if (Fileops_LoadFile(&data, &size, path) != 0 || !data || size == 0) {
        if (data) free(data);
        return;                      /* file missing -> no music, no crash */
    }
    s_musicData = data;
    s_musicSize = (DWORD)size;
    s_musicPos = 0;
    s_musicLoop = loop ? 1 : 0;
    s_carryBytes = 0;
    InterlockedExchange(&s_musicEOF, 0);
    s_musicEofTick = 0;

    /* probe the first frame for channels/hz */
    mp3dec_init(&s_musicDec);
    mp3dec_decode_frame(&s_musicDec, s_musicData, (int)s_musicSize, probe, &info);
    if (!info.hz || !info.channels) {
        free(s_musicData); s_musicData = NULL; return;
    }

    /* re-init the decoder so streaming starts cleanly from frame 0 */
    mp3dec_init(&s_musicDec);
    s_musicPos = 0;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)info.channels;
    wfx.nSamplesPerSec = (DWORD)info.hz;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(info.channels * 2);
    wfx.nAvgBytesPerSec = (DWORD)(info.hz * info.channels * 2);

    /* how long the full ring takes to play -- used as the upper-bound wait for the
       buffered tail to finish after a non-looping track's decoder hits EOF. */
    s_musicDrainMs = wfx.nAvgBytesPerSec
        ? (DWORD)((DWORD)AUDIO_STREAM_BUFSIZE * 1000UL / wfx.nAvgBytesPerSec)
        : 1500;

    ZeroMemory(&dsbd, sizeof(dsbd));
    dsbd.dwSize = sizeof(dsbd);
    dsbd.dwFlags = DSBCAPS_CTRLVOLUME;
    dsbd.dwBufferBytes = AUDIO_STREAM_BUFSIZE;   /* 256KB, not 40MB */
    dsbd.lpwfxFormat = &wfx;

    if (FAILED(s_ds->CreateSoundBuffer(&dsbd, &s_music, NULL)) || !s_music) {
        s_music = NULL;
        free(s_musicData); s_musicData = NULL;
        return;
    }

    FillHalf(0);
    FillHalf(1);
    if (s_music) s_music->SetVolume(s_musicVolDb);   /* apply persisted level */
    s_music->Play(0, 0, DSBPLAY_LOOPING);   /* ring always loops; EOF handled in fill */

    InterlockedExchange(&s_musicStop, 0);
    s_musicThread = CreateThread(NULL, 0, MusicThreadProc, NULL, 0, NULL);
}

void Audio_StopMusic(void) {
    if (s_musicThread) {
        InterlockedExchange(&s_musicStop, 1);
        WaitForSingleObject(s_musicThread, 2000);
        CloseHandle(s_musicThread);
        s_musicThread = NULL;
    }
    InterlockedExchange(&s_musicStop, 0);

    if (s_music) { s_music->Stop(); s_music->Release(); s_music = NULL; }
    if (s_musicData) { free(s_musicData); s_musicData = NULL; }
    s_musicSize = 0; s_musicPos = 0; s_carryBytes = 0; s_levelLow = 0; s_levelHigh = 0; s_lpState = 0;
}

int Audio_DbgReady(void) { return s_ds ? 1 : 0; }

int Audio_DbgSfxLoaded(void) {
    int i, n = 0;
    for (i = 0; i < SFX_COUNT; i++) if (s_sfx[i]) n++;
    return n;
}

int Audio_DbgMusicLoaded(void) { return s_music ? 1 : 0; }

int Audio_DbgFilesRead(void) { return s_dbgRead; }
int Audio_DbgFilesDecoded(void) { return s_dbgDecoded; }

void Audio_Shutdown(void) {
    int i;
    Audio_StopMusic();
    for (i = 0; i < SFX_COUNT; i++) {
        if (s_sfx[i]) { s_sfx[i]->Stop(); s_sfx[i]->Release(); s_sfx[i] = NULL; }
    }
    if (s_csReady) { DeleteCriticalSection(&s_musicCS); s_csReady = 0; }
    if (s_ds) { s_ds->Release(); s_ds = NULL; }
}