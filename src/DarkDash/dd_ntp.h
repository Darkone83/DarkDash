#pragma once
/*---------------------------------------------------------------------------
    dd_ntp.h -- one-shot SNTP time sync.

    Ntp_Sync() queries a time server over UDP (port 123), converts the result
    to UTC and writes it via Sys_SetClockDirect. The Xbox kernel applies the
    EEPROM timezone bias when local time is read back; this code does not modify
    that bias.

    Ntp_AsyncBegin()/Ntp_AsyncPoll() provide the boot-time path: DNS and UDP are
    advanced incrementally from the main loop so a dead resolver/server never
    freezes rendering. The synchronous Ntp_Sync() remains for the explicit
    Settings -> Clock -> Sync Now action.
---------------------------------------------------------------------------*/
#ifndef DD_NTP_H
#define DD_NTP_H

#ifdef __cplusplus
extern "C" {
#endif

    int Ntp_Sync(void);   /* 1 = clock set from internet, 0 = failed */

    int  Ntp_AsyncBegin(void); /* 1 = started, 0 = couldn't start/no network */
    int  Ntp_AsyncPoll(void);  /* 0 = still running, 1 = success, -1 = failed */
    int  Ntp_AsyncBusy(void);  /* 1 while an async attempt is in progress      */
    void Ntp_AsyncCancel(void);/* release any outstanding DNS/socket resources */

#ifdef __cplusplus
}
#endif
#endif /* DD_NTP_H */