/*---------------------------------------------------------------------------
    dd_time.h -- internet-time (NTP) + timezone preferences.

    Stored in its own file (D:\data\time.dat), NOT in settings.dat, so the
    main settings format never has to bump versions for this (same pattern as
    paths.dat). All local -- this NEVER writes the EEPROM timezone bias; the
    offset is applied to NTP-UTC and the result is written to the RTC only.

    The timezone list (name + UTC offset in minutes) is sourced from XbDiag's
    EEPROM timezone table -- we reuse the DATA (names/biases), not its EEPROM
    write code.
---------------------------------------------------------------------------*/
#ifndef DD_TIME_H
#define DD_TIME_H

#ifdef __cplusplus
extern "C" {
#endif

    /* ---- persisted prefs ---------------------------------------------------- */

    void Time_Load(void);                 /* read time.dat (defaults if missing)   */
    void Time_Save(void);                 /* write time.dat                        */

    int  Time_NtpEnabled(void);           /* 1 = sync from internet                */
    void Time_SetNtpEnabled(int on);

    int  Time_TzIndex(void);              /* index into the TZ table               */
    void Time_SetTzIndex(int idx);

    /* ---- timezone table (name + offset) ------------------------------------- */

    int         Tz_Count(void);
    const char* Tz_Name(int idx);         /* e.g. "Mountain Time (US & Canada)"    */
    int         Tz_OffsetMin(int idx);    /* UTC offset in minutes (e.g. -420)     */
    void        Tz_OffsetStr(int idx, char* out, int cap);  /* "UTC-7" / "UTC+5:30" */

#ifdef __cplusplus
}
#endif
#endif /* DD_TIME_H */