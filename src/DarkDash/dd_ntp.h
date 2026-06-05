#pragma once
/*---------------------------------------------------------------------------
    dd_ntp.h -- one-shot SNTP time sync.

    Ntp_Sync() queries a time server over UDP (port 123), converts the result
    to UTC, applies the user's stored timezone offset (dd_time), and writes the
    resulting local wall-clock time to the RTC via Sys_SetClockDirect. It does
    NOT touch the EEPROM timezone bias -- runtime clock only.

    Returns 1 on success (clock updated), 0 on any failure (no network, no
    server reply, bad packet). Non-blocking-ish: uses a short receive timeout so
    it can't hang the dashboard if the server is unreachable.
---------------------------------------------------------------------------*/
#ifndef DD_NTP_H
#define DD_NTP_H

#ifdef __cplusplus
extern "C" {
#endif

    int Ntp_Sync(void);   /* 1 = clock set from internet, 0 = failed */

#ifdef __cplusplus
}
#endif
#endif /* DD_NTP_H */