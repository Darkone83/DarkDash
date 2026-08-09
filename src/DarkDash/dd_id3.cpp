/*---------------------------------------------------------------------------
    dd_id3.cpp -- ID3v2 tag + cover-art reader (see dd_id3.h).
    MSVC2003 / RXDK, C89-style locals. Win32 file I/O + shared stb decoder.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <string.h>
#include <stdlib.h>
#include "dd_id3.h"
#include "dd_stbi.h"

/* ---- byte helpers -------------------------------------------------------- */

static DWORD synchsafe(const unsigned char* p) {
    return ((DWORD)(p[0] & 0x7F) << 21) | ((DWORD)(p[1] & 0x7F) << 14) |
        ((DWORD)(p[2] & 0x7F) << 7) | (DWORD)(p[3] & 0x7F);
}
static DWORD be32(const unsigned char* p) {
    return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) |
        ((DWORD)p[2] << 8) | (DWORD)p[3];
}
static DWORD be24(const unsigned char* p) {
    return ((DWORD)p[0] << 16) | ((DWORD)p[1] << 8) | (DWORD)p[2];
}

/* ID3v2.3 whole-tag unsynchronisation: collapse every FF 00 back to FF. */
static DWORD deunsync(unsigned char* buf, DWORD n) {
    DWORD r = 0, w = 0;
    while (r < n) {
        buf[w++] = buf[r];
        if (buf[r] == 0xFF && r + 1 < n && buf[r + 1] == 0x00) r += 2;
        else r++;
    }
    return w;
}

/* ---- text decoding ------------------------------------------------------- */

static void put_ascii(char* out, int outsz, int* k, int ch) {
    if (ch == 0) return;                 /* drop embedded nulls */
    if (ch < 32 || ch > 126) ch = '?';   /* panel font is ASCII */
    if (*k < outsz - 1) out[(*k)++] = (char)ch;
}

static void decode_text(int enc, const unsigned char* p, DWORD n,
    char* out, int outsz) {
    int k = 0; DWORD i;
    out[0] = 0;
    if (enc == 1) {                      /* UTF-16 with BOM */
        int be = 0; i = 0;
        if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) { be = 1; i = 2; }
        else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) { be = 0; i = 2; }
        for (; i + 1 < n; i += 2)
            put_ascii(out, outsz, &k, be ? ((p[i] << 8) | p[i + 1])
                : ((p[i + 1] << 8) | p[i]));
    }
    else if (enc == 2) {               /* UTF-16BE, no BOM */
        for (i = 0; i + 1 < n; i += 2)
            put_ascii(out, outsz, &k, (p[i] << 8) | p[i + 1]);
    }
    else if (enc == 3) {               /* UTF-8 */
        for (i = 0; i < n; ) {
            unsigned c = p[i];
            if (c < 0x80) { put_ascii(out, outsz, &k, (int)c); i += 1; }
            else if ((c >> 5) == 0x06 && i + 1 < n) { put_ascii(out, outsz, &k, '?'); i += 2; }
            else if ((c >> 4) == 0x0E && i + 2 < n) { put_ascii(out, outsz, &k, '?'); i += 3; }
            else if ((c >> 3) == 0x1E && i + 3 < n) { put_ascii(out, outsz, &k, '?'); i += 4; }
            else i += 1;
        }
    }
    else {                             /* ISO-8859-1 */
        for (i = 0; i < n; i++) put_ascii(out, outsz, &k, p[i]);
    }
    while (k > 0 && out[k - 1] == ' ') k--;   /* trim trailing spaces */
    out[k] = 0;
}

static DWORD a2u(const char* s) {
    DWORD v = 0; int i;
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + (DWORD)(s[i] - '0');
    }
    return v;
}

/* ---- frame text dispatch ------------------------------------------------- */

static void take_text(const unsigned char* p, DWORD n,
    char* dst, int dstsz) {
    if (n < 1) return;
    decode_text(p[0], p + 1, n - 1, dst, dstsz);
}

static void frame_v23(const char* id, const unsigned char* p, DWORD n,
    DD_Id3Info* out) {
    if (n < 1) return;
    if (id[0] == 'T' && id[1] == 'I' && id[2] == 'T' && id[3] == '2') take_text(p, n, out->title, sizeof(out->title));
    else if (id[0] == 'T' && id[1] == 'P' && id[2] == 'E' && id[3] == '1') take_text(p, n, out->artist, sizeof(out->artist));
    else if (id[0] == 'T' && id[1] == 'A' && id[2] == 'L' && id[3] == 'B') take_text(p, n, out->album, sizeof(out->album));
    else if (id[0] == 'T' && id[1] == 'L' && id[2] == 'E' && id[3] == 'N') {
        char tmp[24]; take_text(p, n, tmp, sizeof(tmp)); out->durationMs = a2u(tmp);
    }
}

static void frame_v22(const char* id, const unsigned char* p, DWORD n,
    DD_Id3Info* out) {
    if (n < 1) return;
    if (id[0] == 'T' && id[1] == 'T' && id[2] == '2') take_text(p, n, out->title, sizeof(out->title));
    else if (id[0] == 'T' && id[1] == 'P' && id[2] == '1') take_text(p, n, out->artist, sizeof(out->artist));
    else if (id[0] == 'T' && id[1] == 'A' && id[2] == 'L') take_text(p, n, out->album, sizeof(out->album));
    else if (id[0] == 'T' && id[1] == 'L' && id[2] == 'E') {
        char tmp[24]; take_text(p, n, tmp, sizeof(tmp)); out->durationMs = a2u(tmp);
    }
}

/* skip the extended header (if flagged) and return the body offset of frame 1 */
static DWORD body_start(const unsigned char* body, DWORD bodyLen,
    int ver, unsigned char flags) {
    if (!(flags & 0x40)) return 0;
    if (ver == 4 && bodyLen >= 4) return synchsafe(body);
    if (ver == 3 && bodyLen >= 4) return 4 + be32(body);
    return 0;
}

/* ---- MP3 duration (no TLEN) --------------------------------------------- */

static const int k_br_v1l3[16] = { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0 };
static const int k_br_v2l3[16] = { 0, 8,16,24,32,40,48,56, 64, 80, 96,112,128,144,160,0 };
static const int k_sr_v1[4] = { 44100,48000,32000,0 };
static const int k_sr_v2[4] = { 22050,24000,16000,0 };
static const int k_sr_v25[4] = { 11025,12000, 8000,0 };

static DWORD mp3_duration(HANDLE hf, DWORD audioStart, DWORD fileSize) {
    unsigned char buf[2048];
    DWORD got = 0;
    int i, found = -1;

    if (fileSize == 0 || fileSize == 0xFFFFFFFF || audioStart >= fileSize) return 0;
    if (SetFilePointer(hf, (LONG)audioStart, NULL, FILE_BEGIN) == 0xFFFFFFFF) return 0;
    if (!ReadFile(hf, buf, sizeof(buf), &got, NULL) || got < 4) return 0;

    for (i = 0; i + 1 < (int)got; i++) {
        if (buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0) {
            int v = (buf[i + 1] >> 3) & 3, l = (buf[i + 1] >> 1) & 3;
            if (v != 1 && l == 1) { found = i; break; }   /* MPEG (not reserved), Layer III */
        }
    }
    if (found < 0) return 0;

    {
        const unsigned char* f = buf + found;
        int mpeg = (f[1] >> 3) & 3, brIdx = (f[2] >> 4) & 0xF, srIdx = (f[2] >> 2) & 3;
        int mono = (((f[3] >> 6) & 3) == 3);
        int bitrate, srate, spf;
        DWORD xingOff, frames = 0;

        if (mpeg == 3) { bitrate = k_br_v1l3[brIdx]; srate = k_sr_v1[srIdx];  spf = 1152; }
        else if (mpeg == 2) { bitrate = k_br_v2l3[brIdx]; srate = k_sr_v2[srIdx];  spf = 576; }
        else { bitrate = k_br_v2l3[brIdx]; srate = k_sr_v25[srIdx]; spf = 576; }
        if (bitrate <= 0 || srate <= 0) return 0;

        xingOff = (DWORD)((mpeg == 3) ? (mono ? 17 : 32) : (mono ? 9 : 17));
        {
            const unsigned char* x = f + 4 + xingOff;
            if (x + 12 <= buf + got &&
                ((x[0] == 'X' && x[1] == 'i' && x[2] == 'n' && x[3] == 'g') ||
                    (x[0] == 'I' && x[1] == 'n' && x[2] == 'f' && x[3] == 'o'))) {
                if (be32(x + 4) & 1) frames = be32(x + 8);   /* frames flag */
            }
        }
        if (!frames) {
            const unsigned char* vb = f + 4 + 32;            /* VBRI fixed offset */
            if (vb + 18 <= buf + got &&
                vb[0] == 'V' && vb[1] == 'B' && vb[2] == 'R' && vb[3] == 'I')
                frames = be32(vb + 14);
        }
        if (frames)
            return (DWORD)(((unsigned __int64)frames * (unsigned)spf * 1000ULL) / (unsigned)srate);

        /* CBR estimate: audioBytes*8 bits / (bitrate kbps) == milliseconds */
        return (DWORD)(((unsigned __int64)(fileSize - audioStart) * 8ULL) / (unsigned)bitrate);
    }
}

/* ---- public: read tags + duration --------------------------------------- */

int DD_Id3Read(const char* path, DD_Id3Info* out) {
    HANDLE hf;
    DWORD  fileSize, got = 0, tagSize = 0, audioStart = 0;
    unsigned char hdr[10];
    unsigned char* body = NULL;
    int   ver = 0, filled = 0;
    DWORD attr;

    if (!out) return 0;
    out->title[0] = out->artist[0] = out->album[0] = 0;
    out->durationMs = 0;
    if (!path || !path[0]) return 0;

    attr = GetFileAttributesA(path);
    if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return 0;
    fileSize = GetFileSize(hf, NULL);

    if (ReadFile(hf, hdr, 10, &got, NULL) && got == 10 &&
        hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3' && hdr[3] >= 2 && hdr[3] <= 4) {
        ver = hdr[3];
        tagSize = synchsafe(hdr + 6);
        audioStart = 10 + tagSize;
        if (tagSize > 0 && tagSize < 4UL * 1024 * 1024) {
            body = (unsigned char*)malloc(tagSize);
            if (body && ReadFile(hf, body, tagSize, &got, NULL) && got == tagSize) {
                DWORD bodyLen = tagSize, pos;
                if ((hdr[5] & 0x80) && ver == 3) bodyLen = deunsync(body, bodyLen);
                pos = body_start(body, bodyLen, ver, hdr[5]);
                if (ver == 2) {
                    while (pos + 6 <= bodyLen) {
                        char  id[4]; DWORD fsz;
                        id[0] = (char)body[pos]; id[1] = (char)body[pos + 1]; id[2] = (char)body[pos + 2]; id[3] = 0;
                        if (id[0] == 0) break;
                        fsz = be24(body + pos + 3); pos += 6;
                        if (fsz == 0 || pos + fsz > bodyLen) break;
                        frame_v22(id, body + pos, fsz, out);
                        pos += fsz;
                    }
                }
                else {
                    while (pos + 10 <= bodyLen) {
                        char  id[4]; DWORD fsz;
                        id[0] = (char)body[pos]; id[1] = (char)body[pos + 1];
                        id[2] = (char)body[pos + 2]; id[3] = (char)body[pos + 3];
                        if (id[0] == 0) break;
                        fsz = (ver == 4) ? synchsafe(body + pos + 4) : be32(body + pos + 4);
                        pos += 10;
                        if (fsz == 0 || pos + fsz > bodyLen) break;
                        frame_v23(id, body + pos, fsz, out);
                        pos += fsz;
                    }
                }
            }
            if (body) { free(body); body = NULL; }
        }
    }
    else {
        audioStart = 0;        /* no ID3v2: audio starts at the top of the file */
    }

    if (out->durationMs == 0)
        out->durationMs = mp3_duration(hf, audioStart, fileSize);

    CloseHandle(hf);
    filled = (out->title[0] || out->artist[0] || out->album[0] || out->durationMs) ? 1 : 0;
    return filled;
}

/* ---- public: cover art --------------------------------------------------- */

/* index after the encoding-dependent string terminator within start..n */
static DWORD skip_term(const unsigned char* p, DWORD start, DWORD n, int enc) {
    DWORD i = start;
    if (enc == 1 || enc == 2) {                 /* UTF-16: 00 00 */
        while (i + 1 < n) { if (p[i] == 0 && p[i + 1] == 0) return i + 2; i += 2; }
        return n;
    }
    while (i < n) { if (p[i] == 0) return i + 1; i++; }
    return n;
}

static int find_apic(const unsigned char* body, DWORD bodyLen, int ver,
    unsigned char flags, DWORD* imgOff, DWORD* imgLen) {
    DWORD pos = body_start(body, bodyLen, ver, flags);
    if (ver == 2) {
        while (pos + 6 <= bodyLen) {
            DWORD fsz, fb; int isPic;
            if (body[pos] == 0) break;
            isPic = (body[pos] == 'P' && body[pos + 1] == 'I' && body[pos + 2] == 'C');
            fsz = be24(body + pos + 3); fb = pos + 6;
            if (fsz == 0 || fb + fsz > bodyLen) break;
            if (isPic && fsz > 5) {              /* enc(1)+fmt(3)+type(1)+desc+img */
                int enc = body[fb];
                DWORD d = skip_term(body, fb + 1 + 3 + 1, fb + fsz, enc);
                if (d < fb + fsz) { *imgOff = d; *imgLen = fb + fsz - d; return 1; }
            }
            pos = fb + fsz;
        }
    }
    else {
        while (pos + 10 <= bodyLen) {
            DWORD fsz, fb; int isApic;
            if (body[pos] == 0) break;
            isApic = (body[pos] == 'A' && body[pos + 1] == 'P' && body[pos + 2] == 'I' && body[pos + 3] == 'C');
            fsz = (ver == 4) ? synchsafe(body + pos + 4) : be32(body + pos + 4);
            fb = pos + 10;
            if (fsz == 0 || fb + fsz > bodyLen) break;
            if (isApic && fsz > 4) {             /* enc(1)+mime(z)+type(1)+desc(z)+img */
                int enc = body[fb];
                DWORD d = skip_term(body, fb + 1, fb + fsz, 0);  /* MIME is latin1 */
                if (d < fb + fsz) {
                    d += 1;                                       /* picture type */
                    d = skip_term(body, d, fb + fsz, enc);        /* description  */
                    if (d < fb + fsz) { *imgOff = d; *imgLen = fb + fsz - d; return 1; }
                }
            }
            pos = fb + fsz;
        }
    }
    return 0;
}

static unsigned char* load_image_file(const char* p, int* w, int* h) {
    HANDLE hf; DWORD sz, got = 0, attr; unsigned char* file; unsigned char* rgba;
    attr = GetFileAttributesA(p);
    if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    hf = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return NULL;
    sz = GetFileSize(hf, NULL);
    if (sz == 0 || sz == 0xFFFFFFFF) { CloseHandle(hf); return NULL; }
    file = (unsigned char*)malloc(sz);
    if (!file) { CloseHandle(hf); return NULL; }
    if (!ReadFile(hf, file, sz, &got, NULL) || got != sz) {
        free(file); CloseHandle(hf); return NULL;
    }
    CloseHandle(hf);
    rgba = DD_StbLoadImageMem(file, (int)sz, w, h);
    free(file);
    return rgba;
}

static unsigned char* load_folder_art(const char* path, int* w, int* h) {
    static const char* names[6] =
    { "folder.jpg", "cover.jpg", "front.jpg", "albumart.jpg", "cover.png", "folder.png" };
    char dir[260], cand[260];
    int i, j, k, slash = -1;
    for (i = 0; path[i] && i < 259; i++) { dir[i] = path[i]; if (path[i] == '\\') slash = i; }
    if (slash < 0) return NULL;
    dir[slash + 1] = 0;
    for (i = 0; i < 6; i++) {
        unsigned char* r;
        k = 0;
        for (j = 0; dir[j] && k < 259; j++) cand[k++] = dir[j];
        for (j = 0; names[i][j] && k < 259; j++) cand[k++] = names[i][j];
        cand[k] = 0;
        r = load_image_file(cand, w, h);
        if (r) return r;
    }
    return NULL;
}

unsigned char* DD_Id3LoadArtRGBA(const char* path, int* w, int* h) {
    HANDLE hf; DWORD got = 0, tagSize = 0, attr;
    unsigned char hdr[10]; unsigned char* body = NULL;
    unsigned char* rgba = NULL;
    int ver = 0;

    if (w) *w = 0;
    if (h) *h = 0;
    if (!path || !path[0]) return NULL;

    attr = GetFileAttributesA(path);
    if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        if (ReadFile(hf, hdr, 10, &got, NULL) && got == 10 &&
            hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3' && hdr[3] >= 2 && hdr[3] <= 4) {
            ver = hdr[3];
            tagSize = synchsafe(hdr + 6);
            if (tagSize > 0 && tagSize < 8UL * 1024 * 1024) {
                body = (unsigned char*)malloc(tagSize);
                if (body && ReadFile(hf, body, tagSize, &got, NULL) && got == tagSize) {
                    DWORD bodyLen = tagSize, off = 0, len = 0;
                    if ((hdr[5] & 0x80) && ver == 3) bodyLen = deunsync(body, bodyLen);
                    if (find_apic(body, bodyLen, ver, hdr[5], &off, &len) && len > 0)
                        rgba = DD_StbLoadImageMem(body + off, (int)len, w, h);
                }
                if (body) { free(body); body = NULL; }
            }
        }
        CloseHandle(hf);
    }
    if (rgba) return rgba;
    return load_folder_art(path, w, h);   /* folder.jpg / cover.jpg / ... */
}