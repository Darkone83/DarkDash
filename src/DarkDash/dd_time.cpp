/*---------------------------------------------------------------------------
    dd_time.cpp -- see dd_time.h.

    time.dat blob (magic 'DTIM', ver 1) holds ntpEnabled + tzIndex. The TZ
    table is a condensed name+offset list sourced from XbDiag's EEPROM timezone
    table (we use the data only -- names and UTC-offset-in-minutes -- not the
    44-byte EEPROM blocks or any write code). C89 style, no CRT str*.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_time.h"

#define DDTIME_MAGIC   0x4454494DUL   /* 'DTIM' */
#define DDTIME_VER     1

typedef struct {
    DWORD magic;
    DWORD version;
    int   ntpEnabled;
    int   tzIndex;
} TimeBlob;

static TimeBlob s_t;
static int      s_loaded = 0;

/* Condensed timezone list: { short name, UTC offset in minutes }. Names are
   kept short so the settings row fits inside the console frame (the iso row
   text has no width clip, so we size the content to fit rather than truncate).
   Offsets taken from XbDiag's TZ table (bias negated -> signed UTC offset).
   DST is intentionally not auto-applied -- the user picks the zone matching
   their current wall clock. */
typedef struct { const char* name; int offMin; } TzRow;

static const TzRow s_tz[] = {
    { "Samoa",            -660 },
    { "Hawaii",           -600 },
    { "Alaska",           -540 },
    { "Pacific",          -480 },
    { "Mountain",         -420 },
    { "Arizona",          -420 },
    { "Central",          -360 },
    { "Mexico City",      -360 },
    { "Eastern",          -300 },
    { "Bogota, Lima",     -300 },
    { "Atlantic",         -240 },
    { "Caracas",          -240 },
    { "Santiago",         -240 },
    { "Newfoundland",     -210 },
    { "Brasilia",         -180 },
    { "Buenos Aires",     -180 },
    { "Greenland",        -180 },
    { "Mid-Atlantic",     -120 },
    { "Azores",            -60 },
    { "Cape Verde",        -60 },
    { "London",              0 },
    { "Casablanca",          0 },
    { "Berlin, Rome",       60 },
    { "Paris, Madrid",      60 },
    { "W Central Africa",   60 },
    { "Athens",            120 },
    { "Cairo",             120 },
    { "Jerusalem",         120 },
    { "Helsinki",          120 },
    { "Moscow",            180 },
    { "Baghdad",           180 },
    { "Riyadh",            180 },
    { "Nairobi",           180 },
    { "Tehran",            210 },
    { "Abu Dhabi",         240 },
    { "Baku, Tbilisi",     240 },
    { "Kabul",             270 },
    { "Ekaterinburg",      300 },
    { "Karachi",           300 },
    { "Mumbai, Delhi",     330 },
    { "Kathmandu",         345 },
    { "Almaty, Dhaka",     360 },
    { "Sri Lanka",         360 },
    { "Rangoon",           390 },
    { "Bangkok, Hanoi",    420 },
    { "Beijing, HK",       480 },
    { "Singapore",         480 },
    { "Tokyo, Seoul",      540 },
    { "Adelaide",          570 },
    { "Sydney",            600 },
    { "Guam",              600 },
    { "Solomon Is.",       660 },
    { "Auckland",          720 },
    { "Fiji",              720 }
};
static const int s_tzCount = (int)(sizeof(s_tz) / sizeof(s_tz[0]));

/* compact offset string for display, e.g. "UTC-7", "UTC+5:30", "UTC+0" */
void Tz_OffsetStr(int idx, char* out, int cap) {
    int off, hh, mm, neg, n = 0;
    if (cap < 10) { if (cap > 0) out[0] = 0; return; }
    off = Tz_OffsetMin(idx);
    out[n++] = 'U'; out[n++] = 'T'; out[n++] = 'C';
    neg = (off < 0); if (neg) off = -off;
    hh = off / 60; mm = off % 60;
    out[n++] = neg ? '-' : '+';
    if (hh >= 10) { out[n++] = (char)('0' + hh / 10); }
    out[n++] = (char)('0' + hh % 10);
    if (mm) {
        out[n++] = ':'; out[n++] = (char)('0' + mm / 10); out[n++] = (char)('0' + mm % 10);
    }
    out[n] = 0;
}

/* default zone if none stored: index of "Mountain Time" (sensible US default;
   the user changes it once). */
#define DDTIME_DEFAULT_TZ   4

static void ResetBlob(void) {
    s_t.magic = DDTIME_MAGIC;
    s_t.version = DDTIME_VER;
    s_t.ntpEnabled = 0;
    s_t.tzIndex = DDTIME_DEFAULT_TZ;
}

void Time_Load(void) {
    HANDLE h;
    DWORD  got = 0;
    TimeBlob tmp;

    ResetBlob();
    s_loaded = 1;

    h = CreateFileA("D:\\data\\time.dat", GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (ReadFile(h, &tmp, sizeof(tmp), &got, NULL) && got == sizeof(tmp) &&
        tmp.magic == DDTIME_MAGIC && tmp.version == DDTIME_VER) {
        if (tmp.tzIndex < 0 || tmp.tzIndex >= s_tzCount) tmp.tzIndex = DDTIME_DEFAULT_TZ;
        tmp.ntpEnabled = tmp.ntpEnabled ? 1 : 0;
        s_t = tmp;
    }
    CloseHandle(h);
}

void Time_Save(void) {
    HANDLE h;
    DWORD  wr = 0;
    if (!s_loaded) { ResetBlob(); s_loaded = 1; }
    CreateDirectoryA("D:\\data", NULL);
    h = CreateFileA("D:\\data\\time.dat", GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, &s_t, sizeof(s_t), &wr, NULL);
    CloseHandle(h);
}

int  Time_NtpEnabled(void) { if (!s_loaded) Time_Load(); return s_t.ntpEnabled; }
void Time_SetNtpEnabled(int on) { if (!s_loaded) Time_Load(); s_t.ntpEnabled = on ? 1 : 0; }

int  Time_TzIndex(void) { if (!s_loaded) Time_Load(); return s_t.tzIndex; }
void Time_SetTzIndex(int idx) {
    if (!s_loaded) Time_Load();
    if (idx < 0) idx = s_tzCount - 1;
    if (idx >= s_tzCount) idx = 0;
    s_t.tzIndex = idx;
}

int         Tz_Count(void) { return s_tzCount; }
const char* Tz_Name(int idx) { return (idx >= 0 && idx < s_tzCount) ? s_tz[idx].name : ""; }
int         Tz_OffsetMin(int idx) { return (idx >= 0 && idx < s_tzCount) ? s_tz[idx].offMin : 0; }