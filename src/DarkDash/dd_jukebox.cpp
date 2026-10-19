/*---------------------------------------------------------------------------
    dd_jukebox.cpp -- lightweight nested-folder MP3 browser + playlists.

    Design goals:
      - no boot-time media database; enumerate only the folder being viewed
      - one existing dd_audio music stream owns playback
      - selectable/persisted music root (not tied to the dashboard BGM folder)
      - bounded queues/playlists and shared DD_PATH_MAX path semantics
      - reuse the existing OSK, folder picker, ID3 reader, pedestal and theme UI
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <stdlib.h>
#include <string.h>
#include "dd_jukebox.h"
#include "dd_audio.h"
#include "dd_browse.h"
#include "dd_id3.h"
#include "dd_gfx.h"
#include "dd_iso.h"
#include "dd_osk.h"
#include "dd_path.h"
#include "dd_pedestal.h"
#include "dd_select.h"
#include "dd_stbi.h"
#include "dd_texture.h"
#include "dd_theme.h"
#include "dd_ui.h"
#include "font.h"
#include "input.h"
#include "settings.h"

#define JX_MAGIC             0x31584A44UL /* DJX1 */
#define JX_VERSION           1UL
#define JX_PL_MAGIC          0x31504A44UL /* DJP1 */
#define JX_PL_VERSION        1UL
#define JX_STORE_DIR         "D:\\data\\jukebox"
#define JX_INDEX_FILE        "D:\\data\\jukebox\\index.dat"
#define JX_MAX_ENTRIES       512
#define JX_MAX_PLAYLISTS     16
#define JX_MAX_TRACKS        512
#define JX_NAME_MAX          80
#define JX_PL_NAME_MAX       40
#define JX_VIS               8
#define JX_META_DELAY_MS     120u
#define JX_ENTER_FADE_MS     300u
#define JX_EXIT_FADE_MS      220u
#define JX_TOAST_MS          1800u
#define JX_RECURSE_MAX       8

#define JX_VIEW_LIBRARY      0
#define JX_VIEW_PLAYLISTS    1
#define JX_VIEW_PL_TRACKS    2

#define JX_OSK_NONE          0
#define JX_OSK_NEW_LIST      1
#define JX_OSK_NEW_FOR_ADD   2

#define JX_MENU_X            352.0f
#define JX_MENU_Y            48.0f
#define JX_MENU_W            272.0f
#define JX_MENU_H            384.0f

/* Keep playlist slots stable so deleting one never requires renaming all the
   other files. The display list simply skips slots whose used flag is zero. */
typedef struct {
    char name[JX_PL_NAME_MAX];
    BYTE used;
    BYTE pad[3];
} JxPlRec;

typedef struct {
    DWORD magic;
    DWORD version;
    char  root[DD_PATH_MAX];
    BYTE  shuffle;
    BYTE  pad[3];
    JxPlRec pl[JX_MAX_PLAYLISTS];
} JxIndex;

typedef struct {
    DWORD magic;
    DWORD version;
    DWORD count;
} JxPlHdr;

typedef struct {
    char name[JX_NAME_MAX];
    int  isDir;
} JxEntry;

static int      s_open = 0;
static int      s_view = JX_VIEW_LIBRARY;
static JxIndex  s_idx;
static char     s_path[DD_PATH_MAX];
static JxEntry  s_ent[JX_MAX_ENTRIES];
static int      s_entCount = 0, s_cursor = 0, s_scroll = 0;

/* Playlist screen state. s_plMap translates visible row -> stable slot. */
static int      s_plMap[JX_MAX_PLAYLISTS];
static int      s_plVisible = 0, s_plCursor = 0, s_plScroll = 0;
static int      s_openPl = -1;
static char     s_plTracks[JX_MAX_TRACKS][DD_PATH_MAX];
static int      s_plTrackCount = 0, s_plTrackCursor = 0, s_plTrackScroll = 0;

/* Playback queue. It is intentionally bounded and contains only full paths. */
static char     s_queue[JX_MAX_TRACKS][DD_PATH_MAX];
static int      s_queueCount = 0, s_queuePos = -1;
static int      s_pendingQueuePos = -1;
static int      s_trackActive = 0, s_paused = 0;
static char     s_nowPath[DD_PATH_MAX];
static DD_Id3Info s_nowMeta;
static Texture  s_nowArt;
static DWORD    s_trackStart = 0, s_pauseStart = 0, s_pausedAccum = 0;
static DWORD    s_rng = 0x4A584442UL;

/* Selection metadata/artwork is loaded only after the cursor settles briefly. */
static char     s_selPath[DD_PATH_MAX];
static DD_Id3Info s_selMeta;
static int      s_selMetaValid = 0;
static Texture  s_selArt;
static DWORD    s_selChangedAt = 0;

/* Audio ownership handoff. Settings owns BGM outside Jukebox; Jukebox fades it
   away on entry, stops it, then owns dd_audio until the exit fade completes. */
static int      s_audioReady = 0;
static int      s_savedVol = 70;
static DWORD    s_enterAt = 0;
static int      s_closing = 0;
static DWORD    s_closeAt = 0;
static int      s_closeStartVol = 70;

/* Lightweight overlays/state for playlist add/create/delete. */
static int      s_chooseOpen = 0, s_chooseSel = 0;
static char     s_addPath[DD_PATH_MAX];
static int      s_addIsDir = 0;
static int      s_oskPurpose = JX_OSK_NONE;
static int      s_confirmDelete = 0;
static char     s_toast[96];
static DWORD    s_toastAt = 0;

static int JLen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void JCopy(char* d, int cap, const char* s) {
    int i = 0;
    if (!d || cap <= 0) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int JCmp(const char* a, const char* b) {
    int i = 0;
    for (;;) {
        char ca = a ? a[i] : 0, cb = b ? b[i] : 0;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return (ca < cb) ? -1 : 1;
        if (!ca) return 0;
        i++;
    }
}

static int JPathPrefix(const char* path, const char* root) {
    int i = 0;
    if (!path || !root || !root[0]) return 0;
    while (root[i]) {
        char a = path[i], b = root[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
        i++;
    }
    /* A root ending in '\' already owns everything below it. Otherwise the
       next character must be a separator/end so F:\Music2 can't match F:\Music. */
    if (i > 0 && root[i - 1] == '\\') return 1;
    return (path[i] == 0 || path[i] == '\\') ? 1 : 0;
}
static int JEndsMp3(const char* s) {
    int n = JLen(s);
    if (n < 4 || s[n - 4] != '.') return 0;
    return ((s[n - 3] == 'm' || s[n - 3] == 'M') &&
        (s[n - 2] == 'p' || s[n - 2] == 'P') && s[n - 1] == '3') ? 1 : 0;
}
static const char* JLeaf(const char* p) {
    const char* q = p; const char* last = p;
    if (!p) return "";
    while (*q) { if (*q == '\\') last = q + 1; q++; }
    return last;
}
static void JCleanLeaf(char* out, int cap, const char* p) {
    int n;
    JCopy(out, cap, JLeaf(p));
    n = JLen(out);
    if (n >= 4 && out[n - 4] == '.' &&
        (out[n - 3] == 'm' || out[n - 3] == 'M') &&
        (out[n - 2] == 'p' || out[n - 2] == 'P') && out[n - 1] == '3') out[n - 4] = 0;
}
static void JToast(const char* s) {
    JCopy(s_toast, sizeof(s_toast), s ? s : "");
    s_toastAt = GetTickCount();
    if (s_toastAt == 0) s_toastAt = 1;
}

static void EnsureStore(void) {
    CreateDirectoryA("D:\\data", NULL);
    CreateDirectoryA(JX_STORE_DIR, NULL);
}

static void PlFilePath(int slot, char* out, int cap) {
    char leaf[16];
    if (!out || cap <= 0) return;
    if (slot < 0) slot = 0; if (slot > 99) slot = 99;
    leaf[0] = 'p'; leaf[1] = (char)('0' + (slot / 10)); leaf[2] = (char)('0' + (slot % 10));
    leaf[3] = '.'; leaf[4] = 'd'; leaf[5] = 'p'; leaf[6] = 'l'; leaf[7] = 0;
    DD_PathJoin(out, cap, JX_STORE_DIR, leaf);
}

static int SaveIndex(void) {
    HANDLE h; DWORD wr = 0; int ok;
    EnsureStore();
    s_idx.magic = JX_MAGIC; s_idx.version = JX_VERSION;
    h = CreateFileA(JX_INDEX_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = (WriteFile(h, &s_idx, sizeof(s_idx), &wr, NULL) && wr == sizeof(s_idx)) ? 1 : 0;
    CloseHandle(h);
    return ok;
}

static void LoadIndex(void) {
    HANDLE h; DWORD rd = 0; JxIndex tmp;
    ZeroMemory(&s_idx, sizeof(s_idx));
    s_idx.magic = JX_MAGIC; s_idx.version = JX_VERSION;
    h = CreateFileA(JX_INDEX_FILE, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    ZeroMemory(&tmp, sizeof(tmp));
    if (ReadFile(h, &tmp, sizeof(tmp), &rd, NULL) && rd == sizeof(tmp) &&
        tmp.magic == JX_MAGIC && tmp.version == JX_VERSION) {
        int i;
        s_idx = tmp;
        s_idx.root[DD_PATH_MAX - 1] = 0;
        if (s_idx.root[0] && !DD_PathIsDosAbsolute(s_idx.root)) s_idx.root[0] = 0;
        s_idx.shuffle = s_idx.shuffle ? 1 : 0;
        for (i = 0; i < JX_MAX_PLAYLISTS; i++) {
            s_idx.pl[i].name[JX_PL_NAME_MAX - 1] = 0;
            if (s_idx.pl[i].used && !s_idx.pl[i].name[0]) s_idx.pl[i].used = 0;
        }
    }
    CloseHandle(h);
}

static void BuildPlaylistMap(void) {
    int i;
    s_plVisible = 0;
    for (i = 0; i < JX_MAX_PLAYLISTS; i++)
        if (s_idx.pl[i].used) s_plMap[s_plVisible++] = i;
    if (s_plCursor >= s_plVisible) s_plCursor = (s_plVisible > 0) ? s_plVisible - 1 : 0;
    if (s_plScroll > s_plCursor) s_plScroll = s_plCursor;
}

static int FindFreePlaylistSlot(void) {
    int i; for (i = 0; i < JX_MAX_PLAYLISTS; i++) if (!s_idx.pl[i].used) return i;
    return -1;
}

static int CreatePlaylist(const char* name) {
    int slot = FindFreePlaylistSlot();
    char pf[DD_PATH_MAX]; HANDLE h; DWORD wr = 0; JxPlHdr ph;
    if (slot < 0 || !name || !name[0]) return -1;

    /* Create the backing file first. If storage is unavailable we must not put
       a ghost playlist into index.dat that looks valid but cannot be opened. */
    EnsureStore(); PlFilePath(slot, pf, sizeof(pf));
    ph.magic = JX_PL_MAGIC; ph.version = JX_PL_VERSION; ph.count = 0;
    h = CreateFileA(pf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (!WriteFile(h, &ph, sizeof(ph), &wr, NULL) || wr != sizeof(ph)) {
        CloseHandle(h); DeleteFileA(pf); return -1;
    }
    CloseHandle(h);

    s_idx.pl[slot].used = 1;
    JCopy(s_idx.pl[slot].name, sizeof(s_idx.pl[slot].name), name);
    if (!SaveIndex()) {
        ZeroMemory(&s_idx.pl[slot], sizeof(s_idx.pl[slot]));
        DeleteFileA(pf); return -1;
    }
    BuildPlaylistMap();
    return slot;
}

static int LoadPlaylist(int slot) {
    char pf[DD_PATH_MAX]; HANDLE h; DWORD rd = 0, i; JxPlHdr ph;
    s_plTrackCount = 0;
    if (slot < 0 || slot >= JX_MAX_PLAYLISTS || !s_idx.pl[slot].used) return 0;
    PlFilePath(slot, pf, sizeof(pf));
    h = CreateFileA(pf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 1; /* an empty list is still a valid UI state */
    if (!ReadFile(h, &ph, sizeof(ph), &rd, NULL) || rd != sizeof(ph) ||
        ph.magic != JX_PL_MAGIC || ph.version != JX_PL_VERSION) {
        CloseHandle(h); return 0;
    }
    if (ph.count > JX_MAX_TRACKS) ph.count = JX_MAX_TRACKS;
    for (i = 0; i < ph.count; i++) {
        rd = 0;
        if (!ReadFile(h, s_plTracks[s_plTrackCount], DD_PATH_MAX, &rd, NULL) || rd != DD_PATH_MAX) break;
        s_plTracks[s_plTrackCount][DD_PATH_MAX - 1] = 0;
        if (DD_PathIsDosAbsolute(s_plTracks[s_plTrackCount])) s_plTrackCount++;
    }
    CloseHandle(h);
    return 1;
}

static int SavePlaylist(int slot) {
    char pf[DD_PATH_MAX]; HANDLE h; DWORD wr = 0; int i; JxPlHdr ph;
    if (slot < 0 || slot >= JX_MAX_PLAYLISTS || !s_idx.pl[slot].used) return 0;
    PlFilePath(slot, pf, sizeof(pf)); EnsureStore();
    h = CreateFileA(pf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ph.magic = JX_PL_MAGIC; ph.version = JX_PL_VERSION; ph.count = (DWORD)s_plTrackCount;
    if (!WriteFile(h, &ph, sizeof(ph), &wr, NULL) || wr != sizeof(ph)) { CloseHandle(h); return 0; }
    for (i = 0; i < s_plTrackCount; i++) {
        wr = 0;
        if (!WriteFile(h, s_plTracks[i], DD_PATH_MAX, &wr, NULL) || wr != DD_PATH_MAX) {
            CloseHandle(h); return 0;
        }
    }
    CloseHandle(h); return 1;
}

static int TrackExistsInOpenPlaylist(const char* path) {
    int i; for (i = 0; i < s_plTrackCount; i++) if (JCmp(s_plTracks[i], path) == 0) return 1;
    return 0;
}
static int AppendTrackOpenPlaylist(const char* path) {
    if (!path || !DD_PathIsDosAbsolute(path) || s_plTrackCount >= JX_MAX_TRACKS) return 0;
    if (TrackExistsInOpenPlaylist(path)) return 1;
    if (!DD_PathCopy(s_plTracks[s_plTrackCount], DD_PATH_MAX, path)) return 0;
    s_plTrackCount++; return 1;
}

static void SortPlaylistRange(int first) {
    int i, j;
    if (first < 0) first = 0;
    for (i = first + 1; i < s_plTrackCount; i++) {
        char tmp[DD_PATH_MAX];
        DD_PathCopy(tmp, sizeof(tmp), s_plTracks[i]);
        j = i - 1;
        while (j >= first && JCmp(s_plTracks[j], tmp) > 0) {
            DD_PathCopy(s_plTracks[j + 1], DD_PATH_MAX, s_plTracks[j]);
            j--;
        }
        DD_PathCopy(s_plTracks[j + 1], DD_PATH_MAX, tmp);
    }
}

static void AddFolderRecursive(const char* dir, int depth) {
    WIN32_FIND_DATA fd; HANDLE h; char pat[DD_PATH_PATTERN_MAX]; char full[DD_PATH_MAX];
    if (!dir || depth > JX_RECURSE_MAX || s_plTrackCount >= JX_MAX_TRACKS) return;
    if (!DD_PathJoin(pat, sizeof(pat), dir, "*")) return;
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' && (fd.cFileName[1] == 0 ||
            (fd.cFileName[1] == '.' && fd.cFileName[2] == 0))) continue;
        if (!DD_PathJoin(full, sizeof(full), dir, fd.cFileName)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) AddFolderRecursive(full, depth + 1);
        else if (JEndsMp3(fd.cFileName)) AppendTrackOpenPlaylist(full);
        if (s_plTrackCount >= JX_MAX_TRACKS) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int AddPendingToPlaylist(int slot) {
    int before;
    if (!LoadPlaylist(slot)) return 0;
    before = s_plTrackCount;
    if (s_addIsDir) { AddFolderRecursive(s_addPath, 0); SortPlaylistRange(before); }
    else AppendTrackOpenPlaylist(s_addPath);
    if (!SavePlaylist(slot)) return 0;
    if (s_plTrackCount == before) JToast("Already in playlist / nothing added");
    else JToast("Added to playlist");
    return 1;
}

static void DeletePlaylist(int slot) {
    char pf[DD_PATH_MAX]; JxPlRec old;
    if (slot < 0 || slot >= JX_MAX_PLAYLISTS) return;
    old = s_idx.pl[slot];
    ZeroMemory(&s_idx.pl[slot], sizeof(s_idx.pl[slot]));
    if (!SaveIndex()) {
        s_idx.pl[slot] = old; BuildPlaylistMap();
        JToast("Could not remove playlist"); return;
    }
    PlFilePath(slot, pf, sizeof(pf)); DeleteFileA(pf); BuildPlaylistMap();
    if (s_openPl == slot) { s_openPl = -1; s_plTrackCount = 0; }
    JToast("Playlist removed");
}

static int EntryCmp(const JxEntry* a, const JxEntry* b) {
    if (a->isDir != b->isDir) return a->isDir ? -1 : 1;
    return JCmp(a->name, b->name);
}
static void SortEntries(void) {
    int i, j;
    for (i = 1; i < s_entCount; i++) {
        JxEntry t = s_ent[i]; j = i - 1;
        while (j >= 0 && EntryCmp(&s_ent[j], &t) > 0) { s_ent[j + 1] = s_ent[j]; j--; }
        s_ent[j + 1] = t;
    }
}

static void ReleaseSelectionArt(void) {
    Texture_Release(&s_selArt); ZeroMemory(&s_selMeta, sizeof(s_selMeta));
    s_selMetaValid = 0; s_selPath[0] = 0;
}

static void ReleaseNowArt(void) {
    Texture_Release(&s_nowArt);
}

/* The ID3 layer already uses the shared stb_image wrapper.  It first tries
   embedded APIC/PIC artwork, then folder.jpg / cover.jpg / front.jpg /
   albumart.jpg and PNG equivalents.  Keep a dedicated texture for the
   playing track so browsing never replaces the Now Playing artwork. */
static void LoadNowArt(const char* path) {
    int w = 0, h = 0;
    unsigned char* rgba;
    ReleaseNowArt();
    if (!path || !path[0]) return;
    rgba = DD_Id3LoadArtRGBA(path, &w, &h);
    if (!rgba) return;
    if (w > 0 && h > 0 && w <= 1024 && h <= 1024)
        Texture_LoadRGBA(rgba, w, h, &s_nowArt);
    DD_StbFree(rgba);
    if (s_nowArt.tex) Pedestal_Pulse();
}

static int CurrentSelectionPath(char* out, int cap, int* isDir) {
    if (isDir) *isDir = 0;
    if (s_view == JX_VIEW_LIBRARY) {
        if (s_cursor < 0 || s_cursor >= s_entCount) return 0;
        if (isDir) *isDir = s_ent[s_cursor].isDir;
        return DD_PathJoin(out, cap, s_path, s_ent[s_cursor].name);
    }
    if (s_view == JX_VIEW_PL_TRACKS) {
        if (s_plTrackCursor < 0 || s_plTrackCursor >= s_plTrackCount) return 0;
        return DD_PathCopy(out, cap, s_plTracks[s_plTrackCursor]);
    }
    return 0;
}

static void SelectionChanged(void) {
    ReleaseSelectionArt();
    CurrentSelectionPath(s_selPath, sizeof(s_selPath), NULL);
    s_selChangedAt = GetTickCount(); if (s_selChangedAt == 0) s_selChangedAt = 1;
}

static void LoadSelectionMetaIfReady(void) {
    DWORD now; int isDir = 0, w = 0, h = 0; unsigned char* rgba;
    char p[DD_PATH_MAX];
    if (!s_selChangedAt) return;
    now = GetTickCount(); if (now - s_selChangedAt < JX_META_DELAY_MS) return;
    s_selChangedAt = 0;
    if (!CurrentSelectionPath(p, sizeof(p), &isDir) || isDir) return;
    if (GetFileAttributesA(p) == 0xFFFFFFFF) return;
    /* While music is active the left hero intentionally stays focused on the
       playing track.  Don't spend disk/decode time previewing highlighted art
       that is not going to be drawn. */
    if (s_trackActive) return;
    DD_Id3Read(p, &s_selMeta); s_selMetaValid = 1;
    rgba = DD_Id3LoadArtRGBA(p, &w, &h);
    if (rgba) {
        if (w > 0 && h > 0 && w <= 1024 && h <= 1024)
            Texture_LoadRGBA(rgba, w, h, &s_selArt);
        DD_StbFree(rgba);
        if (s_selArt.tex) Pedestal_Pulse();
    }
}

static void ClampLibrary(void) {
    if (s_cursor < 0) s_cursor = 0;
    if (s_cursor >= s_entCount) s_cursor = (s_entCount > 0) ? s_entCount - 1 : 0;
    if (s_cursor < s_scroll) s_scroll = s_cursor;
    if (s_cursor >= s_scroll + JX_VIS) s_scroll = s_cursor - JX_VIS + 1;
    if (s_scroll < 0) s_scroll = 0;
}
static void ClampPlaylistList(void) {
    if (s_plCursor < 0) s_plCursor = 0;
    if (s_plCursor >= s_plVisible) s_plCursor = (s_plVisible > 0) ? s_plVisible - 1 : 0;
    if (s_plCursor < s_plScroll) s_plScroll = s_plCursor;
    if (s_plCursor >= s_plScroll + JX_VIS) s_plScroll = s_plCursor - JX_VIS + 1;
    if (s_plScroll < 0) s_plScroll = 0;
}
static void ClampPlaylistTracks(void) {
    if (s_plTrackCursor < 0) s_plTrackCursor = 0;
    if (s_plTrackCursor >= s_plTrackCount) s_plTrackCursor = (s_plTrackCount > 0) ? s_plTrackCount - 1 : 0;
    if (s_plTrackCursor < s_plTrackScroll) s_plTrackScroll = s_plTrackCursor;
    if (s_plTrackCursor >= s_plTrackScroll + JX_VIS) s_plTrackScroll = s_plTrackCursor - JX_VIS + 1;
    if (s_plTrackScroll < 0) s_plTrackScroll = 0;
}

static void LoadDir(const char* path) {
    WIN32_FIND_DATA fd; HANDLE h; char pat[DD_PATH_PATTERN_MAX];
    s_entCount = 0; s_cursor = 0; s_scroll = 0;
    if (!path || !path[0] || !DD_PathCopy(s_path, sizeof(s_path), path)) return;
    if (!DD_PathJoin(pat, sizeof(pat), s_path, "*")) return;
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            JxEntry* e;
            if (fd.cFileName[0] == '.' && (fd.cFileName[1] == 0 ||
                (fd.cFileName[1] == '.' && fd.cFileName[2] == 0))) continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !JEndsMp3(fd.cFileName)) continue;
            if (s_entCount >= JX_MAX_ENTRIES) break;
            e = &s_ent[s_entCount++];
            JCopy(e->name, sizeof(e->name), fd.cFileName);
            e->isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    SortEntries(); SelectionChanged();
}

static int PathIsRoot(void) { return (s_idx.root[0] && JCmp(s_path, s_idx.root) == 0) ? 1 : 0; }
static void GoParent(void) {
    int n, i, slash = -1; char parent[DD_PATH_MAX];
    if (PathIsRoot()) return;
    JCopy(parent, sizeof(parent), s_path); n = JLen(parent);
    while (n > 3 && parent[n - 1] == '\\') parent[--n] = 0;
    for (i = 0; i < n; i++) if (parent[i] == '\\') slash = i;
    if (slash < 2) return;
    parent[slash] = 0;
    /* Never allow B navigation above the user-selected Jukebox root. */
    if (JLen(parent) < JLen(s_idx.root) || !JPathPrefix(parent, s_idx.root))
        JCopy(parent, sizeof(parent), s_idx.root);
    LoadDir(parent);
}

static void SetRoot(const char* path) {
    DWORD attr;
    if (!path || !DD_PathIsDosAbsolute(path) || !DD_PathCopy(s_idx.root, sizeof(s_idx.root), path)) return;
    if (!SaveIndex()) JToast("Could not save Jukebox root");
    attr = GetFileAttributesA(s_idx.root);
    if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY)) LoadDir(s_idx.root);
    else { s_path[0] = 0; s_entCount = 0; SelectionChanged(); }
}


static int JRand(int limit) {
    if (limit <= 1) return 0;
    s_rng = s_rng * 1664525UL + 1013904223UL;
    return (int)((s_rng >> 8) % (DWORD)limit);
}

/* Shuffle the active queue in place but keep the user-selected track as the
   initial play position. Previous/Next then walk the same shuffled order, so
   transport remains deterministic for the life of that queue. */
static void ShuffleQueue(const char* chosen) {
    int i, j;
    if (!s_idx.shuffle || s_queueCount < 2) return;
    for (i = s_queueCount - 1; i > 0; i--) {
        char tmp[DD_PATH_MAX];
        j = JRand(i + 1);
        if (i == j) continue;
        DD_PathCopy(tmp, DD_PATH_MAX, s_queue[i]);
        DD_PathCopy(s_queue[i], DD_PATH_MAX, s_queue[j]);
        DD_PathCopy(s_queue[j], DD_PATH_MAX, tmp);
    }
    if (chosen && chosen[0]) {
        for (i = 0; i < s_queueCount; i++) {
            if (JCmp(s_queue[i], chosen) == 0) { s_queuePos = i; break; }
        }
    }
}

static void BuildQueueFromLibrary(const char* chosen) {
    int i; char p[DD_PATH_MAX];
    s_queueCount = 0; s_queuePos = -1;
    for (i = 0; i < s_entCount && s_queueCount < JX_MAX_TRACKS; i++) {
        if (s_ent[i].isDir) continue;
        if (!DD_PathJoin(p, sizeof(p), s_path, s_ent[i].name)) continue;
        if (!DD_PathCopy(s_queue[s_queueCount], DD_PATH_MAX, p)) continue;
        if (chosen && JCmp(chosen, p) == 0) s_queuePos = s_queueCount;
        s_queueCount++;
    }
    ShuffleQueue(chosen);
}
static void BuildQueueFromPlaylist(int chosen) {
    int i; char chosenPath[DD_PATH_MAX];
    chosenPath[0] = 0;
    if (chosen >= 0 && chosen < s_plTrackCount) DD_PathCopy(chosenPath, DD_PATH_MAX, s_plTracks[chosen]);
    s_queueCount = 0; s_queuePos = -1;
    for (i = 0; i < s_plTrackCount && s_queueCount < JX_MAX_TRACKS; i++) {
        if (!DD_PathCopy(s_queue[s_queueCount], DD_PATH_MAX, s_plTracks[i])) continue;
        if (i == chosen) s_queuePos = s_queueCount;
        s_queueCount++;
    }
    ShuffleQueue(chosenPath);
}

static void SyncSelectionToPlaying(const char* path) {
    int i; char p[DD_PATH_MAX];
    if (!path || !path[0]) return;
    if (s_view == JX_VIEW_LIBRARY) {
        for (i = 0; i < s_entCount; i++) {
            if (s_ent[i].isDir) continue;
            if (!DD_PathJoin(p, sizeof(p), s_path, s_ent[i].name)) continue;
            if (JCmp(p, path) == 0) { s_cursor = i; ClampLibrary(); SelectionChanged(); return; }
        }
    }
    else if (s_view == JX_VIEW_PL_TRACKS) {
        for (i = 0; i < s_plTrackCount; i++) {
            if (JCmp(s_plTracks[i], path) == 0) {
                s_plTrackCursor = i; ClampPlaylistTracks(); SelectionChanged(); return;
            }
        }
    }
}

static int StartQueuePos(int pos) {
    DWORD attr;
    if (s_queueCount <= 0) return 0;
    if (pos < 0) pos = s_queueCount - 1;
    if (pos >= s_queueCount) pos = 0;
    if (!s_audioReady) { s_pendingQueuePos = pos; s_queuePos = pos; return 1; }
    s_pendingQueuePos = -1;
    attr = GetFileAttributesA(s_queue[pos]);
    if (attr == 0xFFFFFFFF || (attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    s_queuePos = pos;
    JCopy(s_nowPath, sizeof(s_nowPath), s_queue[s_queuePos]);
    ZeroMemory(&s_nowMeta, sizeof(s_nowMeta)); DD_Id3Read(s_nowPath, &s_nowMeta);
    LoadNowArt(s_nowPath);
    /* StartMusic tears down the prior stream and clears paused state itself.
       Do not briefly resume a paused old track just to stop it again. */
    s_paused = 0; s_trackActive = 0;
    Audio_SetMusicPath(s_nowPath); Audio_SetMusicVolume(s_savedVol); Audio_StartMusic(0);
    s_trackActive = Audio_MusicPlaying();
    s_trackStart = GetTickCount(); if (!s_trackStart) s_trackStart = 1;
    s_pauseStart = 0; s_pausedAccum = 0;
    if (s_trackActive) SyncSelectionToPlaying(s_nowPath);
    return s_trackActive ? 1 : 0;
}
static void NextTrack(int delta) {
    int i, pos, base;
    if (s_queueCount <= 0) return;
    base = (s_queuePos < 0) ? ((delta >= 0) ? -1 : 0) : s_queuePos;
    for (i = 0; i < s_queueCount; i++) {
        pos = (base + delta * (i + 1)) % s_queueCount;
        if (pos < 0) pos += s_queueCount;
        if (StartQueuePos(pos)) return;
    }
    Audio_StopMusic(); s_trackActive = 0; s_nowPath[0] = 0;
    ZeroMemory(&s_nowMeta, sizeof(s_nowMeta)); ReleaseNowArt(); SelectionChanged();
    JToast("No playable tracks in queue");
}

static DWORD TrackElapsed(void) {
    DWORD now, paused = s_pausedAccum;
    if (!s_trackActive || !s_trackStart) return 0;
    now = GetTickCount();
    if (s_paused && s_pauseStart) paused += now - s_pauseStart;
    return now - s_trackStart - paused;
}

static void TogglePause(void) {
    DWORD now;
    if (!s_trackActive) return;
    now = GetTickCount();
    if (!s_paused) {
        Audio_SetMusicPaused(1); s_paused = 1; s_pauseStart = now;
    }
    else {
        Audio_SetMusicPaused(0); s_paused = 0;
        if (s_pauseStart) s_pausedAccum += now - s_pauseStart;
        s_pauseStart = 0;
    }
}

static void OpenRootPicker(void) {
    if (!Browse_IsOpen()) Browse_Open("Jukebox Music Folder");
}

static void OpenAddChooser(const char* path, int isDir) {
    if (!path || !path[0]) return;
    if (!DD_PathCopy(s_addPath, sizeof(s_addPath), path)) return;
    s_addIsDir = isDir; s_chooseSel = 0; s_chooseOpen = 1; BuildPlaylistMap();
}

static void StartNewPlaylistOsk(int forAdd) {
    if (FindFreePlaylistSlot() < 0) { JToast("Playlist limit reached"); return; }
    s_oskPurpose = forAdd ? JX_OSK_NEW_FOR_ADD : JX_OSK_NEW_LIST;
    Osk_Open(OSK_TEXT, "", JX_PL_NAME_MAX - 1);
}

static void ProcessOskResult(int r) {
    char name[JX_PL_NAME_MAX]; int slot;
    if (r == 0) return;
    if (r < 0) { s_oskPurpose = JX_OSK_NONE; return; }
    name[0] = 0; Osk_GetText(name, sizeof(name));
    if (!name[0]) { JToast("Playlist name required"); s_oskPurpose = JX_OSK_NONE; return; }
    slot = CreatePlaylist(name);
    if (slot < 0) JToast("Could not create playlist");
    else if (s_oskPurpose == JX_OSK_NEW_FOR_ADD) AddPendingToPlaylist(slot);
    else JToast("Playlist created");
    s_oskPurpose = JX_OSK_NONE; s_chooseOpen = 0;
}

static void RequestClose(void) {
    if (s_closing) return;
    s_closing = 1; s_closeAt = GetTickCount(); if (!s_closeAt) s_closeAt = 1;
    s_closeStartVol = Audio_GetMusicVolume();
}

void Jukebox_Init(void) {
    s_rng ^= GetTickCount();
    ZeroMemory(&s_selArt, sizeof(s_selArt)); ZeroMemory(&s_nowArt, sizeof(s_nowArt));
    LoadIndex(); BuildPlaylistMap();
}

void Jukebox_Shutdown(void) {
    ReleaseSelectionArt(); ReleaseNowArt();
    if (s_open) { Audio_StopMusic(); }
    s_open = 0;
}

void Jukebox_Enter(void) {
    DWORD attr;
    if (s_open) return;
    s_open = 1; s_view = JX_VIEW_LIBRARY; s_closing = 0; s_confirmDelete = 0;
    s_chooseOpen = 0; s_oskPurpose = JX_OSK_NONE; s_trackActive = 0; s_paused = 0;
    s_queueCount = 0; s_queuePos = -1; s_pendingQueuePos = -1; s_nowPath[0] = 0;
    ZeroMemory(&s_nowMeta, sizeof(s_nowMeta)); ReleaseNowArt();
    s_savedVol = Audio_GetMusicVolume(); if (s_savedVol < 0) s_savedVol = 0; if (s_savedVol > 100) s_savedVol = 100;
    s_audioReady = 0; s_enterAt = GetTickCount(); if (!s_enterAt) s_enterAt = 1;
    Settings_SuspendMusicState();
    BuildPlaylistMap();
    ReleaseSelectionArt();
    if (!s_idx.root[0]) OpenRootPicker();
    else {
        attr = GetFileAttributesA(s_idx.root);
        if (attr != 0xFFFFFFFF && (attr & FILE_ATTRIBUTE_DIRECTORY)) LoadDir(s_idx.root);
        else { s_path[0] = 0; s_entCount = 0; OpenRootPicker(); }
    }
}

int Jukebox_IsOpen(void) { return s_open; }

static int HandleOverlays(WORD pressed) {
    if (Osk_IsOpen()) { ProcessOskResult(Osk_Update(pressed)); return 1; }
    if (Browse_IsOpen()) {
        int r = Browse_Update(pressed);
        if (r == 1) { char p[DD_PATH_MAX]; Browse_GetPath(p, sizeof(p)); SetRoot(p); }
        return 1;
    }
    if (s_confirmDelete) {
        if (pressed & BTN_A) {
            if (s_plVisible > 0) DeletePlaylist(s_plMap[s_plCursor]);
            s_confirmDelete = 0;
        }
        else if (pressed & (BTN_B | BTN_BACK)) s_confirmDelete = 0;
        return 1;
    }
    if (s_chooseOpen) {
        int rows = s_plVisible + 1;
        if (pressed & BTN_DPAD_DOWN) { if (s_chooseSel < rows - 1) { s_chooseSel++; Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_chooseSel > 0) { s_chooseSel--; Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & BTN_A) {
            Audio_PlaySfx(SFX_SELECT);
            if (s_chooseSel < s_plVisible) { AddPendingToPlaylist(s_plMap[s_chooseSel]); s_chooseOpen = 0; }
            else StartNewPlaylistOsk(1);
        }
        if (pressed & (BTN_B | BTN_BACK)) { s_chooseOpen = 0; Audio_PlaySfx(SFX_BACK); }
        return 1;
    }
    return 0;
}

int Jukebox_Update(WORD pressed, WORD held) {
    DWORD now, el; int v;
    (void)held;
    if (!s_open) return 0;
    now = GetTickCount();

    /* Fade the pre-existing dashboard BGM away before Jukebox takes ownership. */
    if (!s_audioReady && !s_closing) {
        el = now - s_enterAt;
        if (el >= JX_ENTER_FADE_MS) {
            int pending;
            Audio_SetMusicVolume(0); Audio_StopMusic(); Audio_SetMusicVolume(s_savedVol); s_audioReady = 1;
            pending = s_pendingQueuePos; s_pendingQueuePos = -1;
            if (pending >= 0 && !StartQueuePos(pending)) NextTrack(1);
        }
        else {
            v = s_savedVol * (int)(JX_ENTER_FADE_MS - el) / (int)JX_ENTER_FADE_MS;
            Audio_SetMusicVolume(v);
        }
    }

    if (s_closing) {
        el = now - s_closeAt;
        if (el >= JX_EXIT_FADE_MS) {
            /* StopMusic is pause-safe; resuming first could leak a tiny burst
               of a paused Jukebox track during the handoff. */
            Audio_StopMusic();
            Audio_SetMusicVolume(s_savedVol);
            Settings_StartMusic(1);   /* hand ownership back to the configured BGM mode */
            ReleaseSelectionArt(); ReleaseNowArt();
            s_open = 0; s_closing = 0; return 1;
        }
        v = s_closeStartVol * (int)(JX_EXIT_FADE_MS - el) / (int)JX_EXIT_FADE_MS;
        if (v < 0) v = 0; Audio_SetMusicVolume(v); return 0;
    }

    if (HandleOverlays(pressed)) { LoadSelectionMetaIfReady(); return 0; }

    if (s_audioReady && s_trackActive && !s_paused && Audio_MusicFinished()) NextTrack(1);

    if (pressed & BTN_START) { TogglePause(); Audio_PlaySfx(SFX_SELECT); }
    if (pressed & BTN_BLACK) { NextTrack(-1); Audio_PlaySfx(SFX_NAV_UP); }
    if (pressed & BTN_WHITE) { NextTrack(1); Audio_PlaySfx(SFX_NAV_DOWN); }
    if (pressed & BTN_BACK) {
        s_idx.shuffle = s_idx.shuffle ? 0 : 1;
        if (!SaveIndex()) JToast("Could not save Shuffle setting");
        else JToast(s_idx.shuffle ? "Shuffle On" : "Shuffle Off");
        Audio_PlaySfx(SFX_ALT);
    }

    if (s_view == JX_VIEW_LIBRARY) {
        if (pressed & BTN_RTHUMB) { OpenRootPicker(); Audio_PlaySfx(SFX_ALT); }
        if (pressed & BTN_DPAD_DOWN) { if (s_cursor < s_entCount - 1) { s_cursor++; ClampLibrary(); SelectionChanged(); Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_cursor > 0) { s_cursor--; ClampLibrary(); SelectionChanged(); Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & BTN_RTRIG) { if (s_cursor < s_entCount - 1) { s_cursor += JX_VIS; ClampLibrary(); SelectionChanged(); Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_LTRIG) { if (s_cursor > 0) { s_cursor -= JX_VIS; ClampLibrary(); SelectionChanged(); Audio_PlaySfx(SFX_NAV_UP); } }
        if ((pressed & BTN_A) && s_entCount > 0) {
            char p[DD_PATH_MAX]; int isDir = 0;
            if (CurrentSelectionPath(p, sizeof(p), &isDir)) {
                Audio_PlaySfx(SFX_SELECT);
                if (isDir) LoadDir(p);
                else {
                    BuildQueueFromLibrary(p);
                    if (s_queuePos >= 0 && !StartQueuePos(s_queuePos)) JToast("Unable to play track");
                }
            }
        }
        if ((pressed & BTN_X) && s_entCount > 0) {
            char p[DD_PATH_MAX]; int isDir = 0;
            if (CurrentSelectionPath(p, sizeof(p), &isDir)) { OpenAddChooser(p, isDir); Audio_PlaySfx(SFX_ALT); }
        }
        if (pressed & BTN_Y) { s_view = JX_VIEW_PLAYLISTS; BuildPlaylistMap(); ReleaseSelectionArt(); Audio_PlaySfx(SFX_ALT); }
        if (pressed & BTN_B) {
            if (!s_idx.root[0] || PathIsRoot()) RequestClose();
            else GoParent();
            Audio_PlaySfx(SFX_BACK);
        }
    }
    else if (s_view == JX_VIEW_PLAYLISTS) {
        if (pressed & BTN_DPAD_DOWN) { if (s_plCursor < s_plVisible - 1) { s_plCursor++; ClampPlaylistList(); Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_plCursor > 0) { s_plCursor--; ClampPlaylistList(); Audio_PlaySfx(SFX_NAV_UP); } }
        if ((pressed & BTN_A) && s_plVisible > 0) {
            s_openPl = s_plMap[s_plCursor]; LoadPlaylist(s_openPl); s_plTrackCursor = 0; s_plTrackScroll = 0;
            s_view = JX_VIEW_PL_TRACKS; SelectionChanged(); Audio_PlaySfx(SFX_SELECT);
        }
        if (pressed & BTN_X) { StartNewPlaylistOsk(0); Audio_PlaySfx(SFX_ALT); }
        if ((pressed & BTN_RTHUMB) && s_plVisible > 0) { s_confirmDelete = 1; Audio_PlaySfx(SFX_ALT); }
        if (pressed & (BTN_Y | BTN_B)) { s_view = JX_VIEW_LIBRARY; SelectionChanged(); Audio_PlaySfx(SFX_BACK); }
    }
    else {
        if (pressed & BTN_DPAD_DOWN) { if (s_plTrackCursor < s_plTrackCount - 1) { s_plTrackCursor++; ClampPlaylistTracks(); SelectionChanged(); Audio_PlaySfx(SFX_NAV_DOWN); } }
        if (pressed & BTN_DPAD_UP) { if (s_plTrackCursor > 0) { s_plTrackCursor--; ClampPlaylistTracks(); SelectionChanged(); Audio_PlaySfx(SFX_NAV_UP); } }
        if (pressed & BTN_RTRIG) { if (s_plTrackCursor < s_plTrackCount - 1) { s_plTrackCursor += JX_VIS; ClampPlaylistTracks(); SelectionChanged(); } }
        if (pressed & BTN_LTRIG) { if (s_plTrackCursor > 0) { s_plTrackCursor -= JX_VIS; ClampPlaylistTracks(); SelectionChanged(); } }
        if ((pressed & BTN_A) && s_plTrackCount > 0) {
            BuildQueueFromPlaylist(s_plTrackCursor);
            if (!StartQueuePos(s_queuePos)) JToast("Unable to play track");
            Audio_PlaySfx(SFX_SELECT);
        }
        if ((pressed & BTN_X) && s_plTrackCount > 0) {
            int i;
            for (i = s_plTrackCursor; i < s_plTrackCount - 1; i++)
                DD_PathCopy(s_plTracks[i], DD_PATH_MAX, s_plTracks[i + 1]);
            s_plTrackCount--; SavePlaylist(s_openPl); ClampPlaylistTracks(); SelectionChanged(); JToast("Removed from playlist");
        }
        if (pressed & (BTN_Y | BTN_B)) { s_view = JX_VIEW_PLAYLISTS; ReleaseSelectionArt(); Audio_PlaySfx(SFX_BACK); }
    }

    /* L3 is both the main-menu entry button and a quick way back out. It is
       intentionally ignored while an OSK/folder picker owns input. */
    if (pressed & BTN_LTHUMB) RequestClose();
    LoadSelectionMetaIfReady();
    return 0;
}

static DWORD MulA(DWORD c, int a) {
    int ca = (int)((c >> 24) & 255); ca = ca * a / 255;
    return ((DWORD)ca << 24) | (c & 0x00FFFFFF);
}
static void DrawOutline(float x, float y, float w, float h, DWORD c) {
    UI_FillRect(x, y, w, 1.0f, c); UI_FillRect(x, y + h - 1.0f, w, 1.0f, c);
    UI_FillRect(x, y, 1.0f, h, c); UI_FillRect(x + w - 1.0f, y, 1.0f, h, c);
}
static void DrawTime(char* out, int cap, DWORD ms) {
    DWORD sec = ms / 1000, min = sec / 60; int n = 0;
    if (!out || cap < 6) return;
    if (min > 99) min = 99; sec %= 60;
    if (min >= 10) out[n++] = (char)('0' + (min / 10));
    out[n++] = (char)('0' + (min % 10)); out[n++] = ':';
    out[n++] = (char)('0' + (sec / 10)); out[n++] = (char)('0' + (sec % 10)); out[n] = 0;
}

static void DrawLeftInfo(IDirect3DDevice8* d, DWORD accent, DWORD text, DWORD dim) {
    int ar = (int)((accent >> 16) & 255), ag = (int)((accent >> 8) & 255), ab = (int)(accent & 255);
    const DD_Id3Info* meta = NULL;
    const Texture* art = NULL;
    const char* sourcePath = NULL;
    const char* label = NULL;
    char title[80], tm[16], dur[16];
    DWORD elapsed = 0, total = 0;

    /* The hero column has one job at a time.  Playback wins over selection so
       browsing cannot make Now Playing show metadata/art for another file. */
    if (s_trackActive) {
        meta = &s_nowMeta; art = s_nowArt.tex ? &s_nowArt : NULL;
        sourcePath = s_nowPath; label = s_paused ? "PAUSED" : "NOW PLAYING";
        elapsed = TrackElapsed(); total = s_nowMeta.durationMs;
    }
    else if (s_selMetaValid && s_selPath[0]) {
        meta = &s_selMeta; art = s_selArt.tex ? &s_selArt : NULL;
        sourcePath = s_selPath; label = "TRACK INFO";
        total = s_selMeta.durationMs;
    }

    if (art) Pedestal_DrawFlat(art, GetTickCount(), ar, ag, ab);
    if (!meta || !sourcePath || !sourcePath[0]) return;

    title[0] = 0;
    if (meta->title[0]) JCopy(title, sizeof(title), meta->title);
    else JCleanLeaf(title, sizeof(title), sourcePath); /* filename is fallback only */

    /* A taller, single-purpose card replaces the old selected-track block plus
       cramped inline Now Playing row.  ID3 gets the visual hierarchy. */
    UI_DrawPanelDepth(38.0f, 292.0f, 280.0f, 146.0f, 255);
    UI_FillRect(38.0f, 292.0f, 280.0f, 146.0f, 0xC8101418);
    UI_DrawPanelLighting(38.0f, 292.0f, 280.0f, 146.0f, 255);
    DrawOutline(38.0f, 292.0f, 280.0f, 146.0f, MulA(accent, 100));

    Font_DrawText(d, 52.0f, 304.0f, label, FONT_SIZE_SMALL, accent, 252);
    if (title[0]) Font_DrawText(d, 52.0f, 327.0f, title, FONT_SIZE_MEDIUM, text, 252);
    if (meta->artist[0]) Font_DrawText(d, 52.0f, 354.0f, meta->artist, FONT_SIZE_SMALL, text, 252);
    if (meta->album[0]) Font_DrawText(d, 52.0f, 376.0f, meta->album, FONT_SIZE_SMALL, dim, 252);

    if (s_trackActive) {
        DrawTime(tm, sizeof(tm), elapsed);
        DrawTime(dur, sizeof(dur), total);
        Font_DrawText(d, 52.0f, 407.0f, tm, FONT_SIZE_SMALL, dim, 0);
        if (total) {
            Font_DrawText(d, 91.0f, 407.0f, "/", FONT_SIZE_SMALL, dim, 0);
            Font_DrawText(d, 103.0f, 407.0f, dur, FONT_SIZE_SMALL, dim, 0);
            {
                float w = 142.0f;
                float p = (elapsed >= total) ? 1.0f : ((float)elapsed / (float)total);
                UI_FillRect(160.0f, 413.0f, w, 4.0f, 0x50404040);
                UI_FillRect(160.0f, 413.0f, w * p, 4.0f, accent);
            }
        }
    }
    else if (total) {
        DrawTime(dur, sizeof(dur), total);
        Font_DrawText(d, 52.0f, 407.0f, dur, FONT_SIZE_SMALL, dim, 0);
    }
}

static void DrawRightPanel(IDirect3DDevice8* d, DWORD accent, DWORD text, DWORD dim, DWORD glow) {
    const Texture* menu = Theme_Asset("frame_menu_v"); int i, start, count, cur;
    float rowY0 = 132.0f, rowDY = 31.0f, gy;
    int gr = (int)((glow >> 16) & 255), gg = (int)((glow >> 8) & 255), gb = (int)(glow & 255);

    Iso_Begin();
    /* Mask the existing main-menu text/highlight, then redraw the same chrome so
       the arcs from DrawSplash remain visible around the frame perimeter. */
    Iso_FillRect(JX_MENU_X + 8.0f, JX_MENU_Y + 8.0f, JX_MENU_W - 16.0f, JX_MENU_H - 16.0f, 0xF0101418, 0);
    if (menu) Iso_DrawPanel(menu, JX_MENU_X, JX_MENU_Y, JX_MENU_W, JX_MENU_H, 0xFFFFFFFF, 0);

    Font_DrawTextIso(d, JX_MENU_X + 26.0f, 72.0f, "JUKEBOX", FONT_SIZE_MEDIUM, accent);
    if (s_idx.shuffle) Font_DrawTextIso(d, JX_MENU_X + 174.0f, 76.0f, "SHUFFLE", FONT_SIZE_SMALL, dim);
    if (s_view == JX_VIEW_LIBRARY) {
        const char* rp = s_path[0] ? s_path : (s_idx.root[0] ? s_idx.root : "NO MUSIC ROOT");
        Font_DrawTextIsoClip(d, JX_MENU_X + 26.0f, 104.0f, rp, FONT_SIZE_SMALL, dim, 214.0f);
        start = s_scroll; count = s_entCount; cur = s_cursor;
        for (i = 0; i < JX_VIS && start + i < count; i++) {
            char lab[JX_NAME_MAX + 2]; int ri = start + i; DWORD c = (ri == cur) ? glow : text;
            JCopy(lab, sizeof(lab), s_ent[ri].name);
            if (s_ent[ri].isDir && JLen(lab) < (int)sizeof(lab) - 2) { int n = JLen(lab); lab[n++] = '\\'; lab[n] = 0; }
            Font_DrawTextIsoClip(d, JX_MENU_X + 28.0f, rowY0 + rowDY * (float)i, lab, FONT_SIZE_SMALL, c, 210.0f);
        }
        if (count <= 0) Font_DrawTextIso(d, JX_MENU_X + 28.0f, rowY0, "(no MP3s / folders)", FONT_SIZE_SMALL, dim);
        gy = rowY0 + rowDY * (float)(cur - start) - 5.0f;
    }
    else if (s_view == JX_VIEW_PLAYLISTS) {
        Font_DrawTextIso(d, JX_MENU_X + 26.0f, 104.0f, "PLAYLISTS", FONT_SIZE_SMALL, dim);
        start = s_plScroll; count = s_plVisible; cur = s_plCursor;
        for (i = 0; i < JX_VIS && start + i < count; i++) {
            int slot = s_plMap[start + i]; DWORD c = ((start + i) == cur) ? glow : text;
            Font_DrawTextIsoClip(d, JX_MENU_X + 28.0f, rowY0 + rowDY * (float)i,
                s_idx.pl[slot].name, FONT_SIZE_SMALL, c, 210.0f);
        }
        if (count <= 0) Font_DrawTextIso(d, JX_MENU_X + 28.0f, rowY0, "(X creates a playlist)", FONT_SIZE_SMALL, dim);
        gy = rowY0 + rowDY * (float)(cur - start) - 5.0f;
    }
    else {
        const char* nm = (s_openPl >= 0 && s_openPl < JX_MAX_PLAYLISTS) ? s_idx.pl[s_openPl].name : "PLAYLIST";
        Font_DrawTextIsoClip(d, JX_MENU_X + 26.0f, 104.0f, nm, FONT_SIZE_SMALL, dim, 214.0f);
        start = s_plTrackScroll; count = s_plTrackCount; cur = s_plTrackCursor;
        for (i = 0; i < JX_VIS && start + i < count; i++) {
            char lab[80]; DWORD c = ((start + i) == cur) ? glow : text;
            JCleanLeaf(lab, sizeof(lab), s_plTracks[start + i]);
            if (GetFileAttributesA(s_plTracks[start + i]) == 0xFFFFFFFF) c = dim;
            Font_DrawTextIsoClip(d, JX_MENU_X + 28.0f, rowY0 + rowDY * (float)i, lab, FONT_SIZE_SMALL, c, 210.0f);
        }
        if (count <= 0) Font_DrawTextIso(d, JX_MENU_X + 28.0f, rowY0, "(empty playlist)", FONT_SIZE_SMALL, dim);
        gy = rowY0 + rowDY * (float)(cur - start) - 5.0f;
    }

    if (count > 0 && Theme_GlowEnabled()) {
        Select_Begin(0x4A58 + s_view, gy);
        Select_DrawGlow(JX_MENU_X + 18.0f, gy, 224.0f, 27.0f,
            UI_ARGB(85 * Theme_GlowIntensity() / 100, gr, gg, gb));
    }
    Iso_End();
}

static void DrawFooter(IDirect3DDevice8* d, DWORD text) {
    const Texture* foot = Theme_Asset("bar_footer"); const char* hint;
    float gh, ty;
    if (foot) UI_DrawSprite(foot, 8.0f, 442.0f, 624.0f, 32.0f, 0xFFFFFFFF, 0);
    if (s_view == JX_VIEW_LIBRARY)
        hint = "A OPEN/PLAY  X ADD  Y LISTS  START PAUSE  BACK SHUFFLE  R3 ROOT";
    else if (s_view == JX_VIEW_PLAYLISTS)
        hint = "A OPEN  X NEW  R3 DELETE  Y/B LIBRARY  BACK SHUFFLE";
    else
        hint = "A PLAY  X REMOVE  Y/B BACK  START PAUSE  BACK SHUFFLE";
    if (Font_MeasureText(hint, FONT_SIZE_SMALL) > 590) {
        if (s_view == JX_VIEW_LIBRARY) hint = "A PLAY  X ADD  Y LISTS  START PAUSE  R3 ROOT";
        else if (s_view == JX_VIEW_PLAYLISTS) hint = "A OPEN  X NEW  R3 DELETE  Y/B BACK";
        else hint = "A PLAY  X REMOVE  Y/B BACK  START PAUSE";
    }
    gh = (float)Font_GlyphHeight(FONT_SIZE_SMALL); ty = 442.0f + (32.0f - gh) * 0.5f; if (ty < 444.0f) ty = 444.0f;
    Font_DrawText(d, 24.0f, ty, hint, FONT_SIZE_SMALL, text, 0);
}

static void DrawChooser(IDirect3DDevice8* d, DWORD accent, DWORD text, DWORD dim) {
    float x = 192.0f, y = 108.0f, w = 256.0f, h = 264.0f; int i, rows = s_plVisible + 1;
    int start = (s_chooseSel >= 8) ? (s_chooseSel - 7) : 0;
    UI_FillRect(0, 0, 640, 480, 0xA8000000);
    UI_DrawPanelDepth(x, y, w, h, 255);
    UI_FillRect(x, y, w, h, 0xF0101418); DrawOutline(x, y, w, h, dim);
    UI_DrawPanelLighting(x, y, w, h, 255);
    Font_DrawText(d, x + 16, y + 14, "ADD TO PLAYLIST", FONT_SIZE_SMALL, accent, 0);
    for (i = 0; i < 8 && start + i < rows; i++) {
        int ri = start + i;
        const char* nm = (ri < s_plVisible) ? s_idx.pl[s_plMap[ri]].name : "+ NEW PLAYLIST";
        float ry = y + 48.0f + (float)i * 24.0f;
        if (ri == s_chooseSel) UI_FillRect(x + 10, ry - 3, w - 20, 22, MulA(accent, 70));
        Font_DrawText(d, x + 18, ry, nm, FONT_SIZE_SMALL, (ri == s_chooseSel) ? text : dim, (int)w - 36);
    }
    Font_DrawText(d, x + 16, y + h - 26, "A ADD   B CANCEL", FONT_SIZE_SMALL, dim, 0);
}
static void DrawDeleteConfirm(IDirect3DDevice8* d, DWORD accent, DWORD text, DWORD dim) {
    float x = 184.0f, y = 174.0f, w = 272.0f, h = 126.0f;
    const char* nm = (s_plVisible > 0) ? s_idx.pl[s_plMap[s_plCursor]].name : "playlist";
    UI_FillRect(0, 0, 640, 480, 0xA8000000);
    UI_DrawPanelDepth(x, y, w, h, 255);
    UI_FillRect(x, y, w, h, 0xF0101418); DrawOutline(x, y, w, h, accent);
    UI_DrawPanelLighting(x, y, w, h, 255);
    Font_DrawText(d, x + 16, y + 16, "REMOVE PLAYLIST?", FONT_SIZE_SMALL, accent, 0);
    Font_DrawText(d, x + 16, y + 46, nm, FONT_SIZE_SMALL, text, (int)w - 32);
    Font_DrawText(d, x + 16, y + 88, "A REMOVE   B CANCEL", FONT_SIZE_SMALL, dim, 0);
}

void Jukebox_Render(void) {
    IDirect3DDevice8* d; DWORD accent, text, dim, glow;
    if (!s_open) return;
    d = Gfx_Device(); accent = Theme_Color("accent", 0xFF7FE000); text = Theme_Color("text", 0xFFD8F8C0);
    dim = Theme_Color("text_dim", 0xFF7FA060); glow = Theme_GlowColor();
    DrawLeftInfo(d, accent, text, dim); DrawRightPanel(d, accent, text, dim, glow); DrawFooter(d, text);
    if (s_toastAt && GetTickCount() - s_toastAt < JX_TOAST_MS) {
        int a = 255 - (int)((GetTickCount() - s_toastAt) * 160 / JX_TOAST_MS); if (a < 80) a = 80;
        UI_FillRect(196.0f, 58.0f, 248.0f, 28.0f, UI_ARGB(a * 3 / 4, 8, 12, 14));
        Font_DrawText(d, 208.0f, 64.0f, s_toast, FONT_SIZE_SMALL, MulA(accent, a), 224);
    }
    if (s_chooseOpen) DrawChooser(d, accent, text, dim);
    if (s_confirmDelete) DrawDeleteConfirm(d, accent, text, dim);
    if (Browse_IsOpen()) Browse_Draw(d);
    if (Osk_IsOpen()) Osk_Draw(d);
}