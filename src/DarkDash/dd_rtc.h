#ifndef DD_RTC_H
#define DD_RTC_H
/*---------------------------------------------------------------------------
    dd_rtc.h -- X-RTC support (optional battery-backed RTC on the SMBus).

    A DS1307-class RTC at SMBus address 0xD0 (7-bit 0x68), as used by the
    Darkone Customs X-RTC. The stock Xbox has no battery clock, so time is
    lost on power-off; an X-RTC persists it. When one is present we mirror
    every clock change (manual or NTP) to it; when absent, every entry point
    is a safe no-op and the dashboard falls back to system-time-only.

    ALL bus access goes through the SMBus broker (Smb_Read8 / Smb_Write8) so
    it is serialized with the service thread -- never raw Hal calls. Stored
    time is UTC (the kernel keeps UTC; the timezone is applied at display).
---------------------------------------------------------------------------*/
#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    void Rtc_Probe(void);                  /* one-shot presence probe; call once after Smb_Ready() */
    int  Rtc_Present(void);                /* 1 if an X-RTC was detected and looks valid, else 0   */

    void Rtc_WriteUtc(const FILETIME* utc);/* persist a UTC time to the X-RTC. No-op if absent.    */
    int  Rtc_ReadUtc(FILETIME* utc);       /* read the X-RTC's UTC time. 1 = ok, 0 = absent/invalid */

#ifdef __cplusplus
}
#endif
#endif /* DD_RTC_H */