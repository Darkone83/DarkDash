#pragma once
/*---------------------------------------------------------------------------
    dd_path.h -- common DarkDash DOS-path sizing + exact-copy helpers.

    User-visible filesystem paths share one capacity so launcher scans, custom
    categories, FileMan, copy jobs, FTP, ISO tools, and XBE launching don't
    disagree about what a valid path looks like. A path operation either fits
    completely or fails; helpers never return a silently-truncated path.
---------------------------------------------------------------------------*/
#ifndef DD_PATH_H
#define DD_PATH_H

#define DD_PATH_MAX          260
#define DD_PATH_PATTERN_MAX  (DD_PATH_MAX + 4)

static int DD_PathLen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

/* Exact copy. Returns 1 only when the entire source (plus NUL) fits. On
   failure dst is cleared so callers can never accidentally use a truncation. */
static int DD_PathCopy(char* dst, int cap, const char* src) {
    int i = 0;
    if (!dst || cap <= 0) return 0;
    dst[0] = 0;
    if (!src) return 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    if (src[i]) { dst[0] = 0; return 0; }
    dst[i] = 0;
    return 1;
}

/* Exact DOS path join: base + optional '\\' + leaf. Returns 0 and clears out
   if the complete result does not fit. */
static int DD_PathJoin(char* out, int cap, const char* base, const char* leaf) {
    int n = 0, i = 0;
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!base || !leaf) return 0;

    while (base[n] && n < cap - 1) { out[n] = base[n]; n++; }
    if (base[n]) { out[0] = 0; return 0; }

    if (n > 0 && out[n - 1] != '\\') {
        if (n >= cap - 1) { out[0] = 0; return 0; }
        out[n++] = '\\';
    }

    while (leaf[i] && n < cap - 1) out[n++] = leaf[i++];
    if (leaf[i]) { out[0] = 0; return 0; }
    out[n] = 0;
    return 1;
}

static int DD_PathIsDosAbsolute(const char* path) {
    if (!path) return 0;
    return (path[0] && path[1] == ':' && path[2] == '\\') ? 1 : 0;
}

#endif /* DD_PATH_H */