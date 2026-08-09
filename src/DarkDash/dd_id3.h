#ifndef DARKDASH_ID3_H
#define DARKDASH_ID3_H
/*---------------------------------------------------------------------------
    dd_id3 -- minimal ID3v2 reader for the Now Playing screen.

    Parses ID3v2.2 / v2.3 / v2.4 text frames (title / artist / album) and a
    best-effort track duration (TLEN -> Xing/Info/VBRI -> CBR estimate). Also
    pulls embedded cover art (APIC / PIC), falling back to folder.jpg /
    cover.jpg in the track's directory. Text is flattened to printable ASCII
    (the panel font is ASCII); non-ASCII code points become '?'.

    All file I/O is Win32/Xbox (CreateFileA/ReadFile); art is decoded with the
    shared stb wrapper (DD_StbLoadImageMem / DD_StbFree).
---------------------------------------------------------------------------*/
#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    typedef struct {
        char  title[64];
        char  artist[64];
        char  album[64];
        DWORD durationMs;        /* 0 when it can't be determined */
    } DD_Id3Info;

    /* Parse tags + duration from <path>. Fields are cleared first. Returns 1 if
       anything was filled (a tag value or a duration), 0 otherwise. */
    int DD_Id3Read(const char* path, DD_Id3Info* out);

    /* Embedded cover art (first APIC/PIC), else folder.jpg / cover.jpg / ... in
       the same directory, as a malloc'd RGBA8 buffer (free with DD_StbFree).
       Fills *w,*h. Returns NULL when no art is available. */
    unsigned char* DD_Id3LoadArtRGBA(const char* path, int* w, int* h);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_ID3_H */