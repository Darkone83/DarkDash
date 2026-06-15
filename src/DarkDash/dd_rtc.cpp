/*---------------------------------------------------------------------------
    dd_rtc.cpp -- see dd_rtc.h.

    DS1307-class register map (BCD):
       0 seconds (bit7 = CH, clock-halt)   1 minutes        2 hours (bit6=12/24)
       3 day-of-week (1..7, not BCD)        4 day            5 month
       6 year (00..99, +2000)

    Build: MSVC2003 / C89 style; file-scope statics; access via the broker only.
---------------------------------------------------------------------------*/
#include "dd_rtc.h"
#include "dd_smbus.h"

#define RTC_ADDR8   0xD0       /* 7-bit 0x68 */
#define RTC_SEC     0
#define RTC_MIN     1
#define RTC_HOUR    2
#define RTC_DOW     3
#define RTC_DAY     4
#define RTC_MONTH   5
#define RTC_YEAR    6
#define RTC_CH      0x80       /* clock-halt bit, in the seconds register */

static int s_probed = 0;
static int s_present = 0;

static unsigned char Bcd2Bin(unsigned char v) { return (unsigned char)(((v >> 4) * 10) + (v & 0x0f)); }
static unsigned char Bin2Bcd(unsigned char v) { return (unsigned char)(((v / 10) << 4) | (v % 10)); }

/* One-shot: detect an X-RTC. We read the time registers and sanity-check the
   BCD ranges, so a device that merely ACKs at 0xD0 but returns garbage is NOT
   mistaken for an RTC (and we never write to a non-RTC responder). */
void Rtc_Probe(void) {
    unsigned char sec, mins, hr, day, mon;
    if (s_probed) return;
    s_probed = 1;
    s_present = 0;
    if (!Smb_Read8(RTC_ADDR8, RTC_SEC, &sec))  return;
    if (!Smb_Read8(RTC_ADDR8, RTC_MIN, &mins)) return;
    if (!Smb_Read8(RTC_ADDR8, RTC_HOUR, &hr))   return;
    if (!Smb_Read8(RTC_ADDR8, RTC_DAY, &day))  return;
    if (!Smb_Read8(RTC_ADDR8, RTC_MONTH, &mon))  return;
    if (Bcd2Bin((unsigned char)(sec & 0x7f)) > 59) return;
    if (Bcd2Bin(mins) > 59)                        return;
    if (Bcd2Bin((unsigned char)(hr & 0x3f)) > 23)  return;
    {
        unsigned char d = Bcd2Bin(day), m = Bcd2Bin(mon);
        if (d < 1 || d > 31 || m < 1 || m > 12)    return;
    }
    s_present = 1;
}

int Rtc_Present(void) { return s_present; }

void Rtc_WriteUtc(const FILETIME* utc) {
    SYSTEMTIME st;
    FILETIME   f;
    if (!s_present || !utc) return;
    f = *utc;
    if (!FileTimeToSystemTime(&f, &st)) return;
    /* writing valid BCD seconds also clears CH (bit7=0), so the oscillator runs */
    Smb_Write8(RTC_ADDR8, RTC_SEC, Bin2Bcd((unsigned char)st.wSecond));
    Smb_Write8(RTC_ADDR8, RTC_MIN, Bin2Bcd((unsigned char)st.wMinute));
    Smb_Write8(RTC_ADDR8, RTC_HOUR, Bin2Bcd((unsigned char)st.wHour));   /* 24h: bit6 = 0 */
    Smb_Write8(RTC_ADDR8, RTC_DOW, (unsigned char)st.wDayOfWeek);
    Smb_Write8(RTC_ADDR8, RTC_DAY, Bin2Bcd((unsigned char)st.wDay));
    Smb_Write8(RTC_ADDR8, RTC_MONTH, Bin2Bcd((unsigned char)st.wMonth));
    Smb_Write8(RTC_ADDR8, RTC_YEAR, Bin2Bcd((unsigned char)(st.wYear % 100)));
}

int Rtc_ReadUtc(FILETIME* utc) {
    SYSTEMTIME st;
    unsigned char v;
    if (!s_present || !utc) return 0;
    ZeroMemory(&st, sizeof(st));
    if (!Smb_Read8(RTC_ADDR8, RTC_SEC, &v)) return 0;  st.wSecond = Bcd2Bin((unsigned char)(v & 0x7f));
    if (!Smb_Read8(RTC_ADDR8, RTC_MIN, &v)) return 0;  st.wMinute = Bcd2Bin(v);
    if (!Smb_Read8(RTC_ADDR8, RTC_HOUR, &v)) return 0;  st.wHour = Bcd2Bin((unsigned char)(v & 0x3f));
    if (!Smb_Read8(RTC_ADDR8, RTC_DOW, &v)) return 0;  st.wDayOfWeek = (WORD)(v & 0x07);
    if (!Smb_Read8(RTC_ADDR8, RTC_DAY, &v)) return 0;  st.wDay = Bcd2Bin(v);
    if (!Smb_Read8(RTC_ADDR8, RTC_MONTH, &v)) return 0;  st.wMonth = Bcd2Bin(v);
    if (!Smb_Read8(RTC_ADDR8, RTC_YEAR, &v)) return 0;  st.wYear = (WORD)(2000 + Bcd2Bin(v));
    st.wMilliseconds = 0;
    if (st.wMonth < 1 || st.wMonth > 12 || st.wDay < 1 || st.wDay > 31) return 0;
    return SystemTimeToFileTime(&st, utc) ? 1 : 0;
}