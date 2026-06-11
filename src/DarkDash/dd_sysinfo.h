/*---------------------------------------------------------------------------
    dd_sysinfo.h -- board sensors + disk free, for the status scroller.
    Ported from XbDiag (TempMonitor / SysInfo / HddInfo).
---------------------------------------------------------------------------*/
#ifndef DD_SYSINFO_H
#define DD_SYSINFO_H

void Sys_Init(void);                              /* clear stuck SMBus once  */
void Sys_SmbusReset(void);                         /* W1C-clear nForce SMBus (call before probing on OC/softmod) */
int  Sys_ReadTemps(int* cpuC, int* boardC);       /* 1 if read, degrees C    */
int  Sys_ReadFanPct(int* pct);                    /* 1 if read, 0..100       */
void Sys_ServiceSensors(void);                    /* bus refill; SERVICE THREAD ONLY */
void Sys_DiskFreeStr(const char* drive, char* out, int cap);  /* "12.3 GB"   */
void Sys_DiskUsageStr(const char* drive, char* out, int cap); /* "4.2 / 8.0 GB" */

/* Fan override (SMC PIC). Each tries the bus write and returns 1 on success,
   0 if the write was refused (e.g. xemu) -- caller continues either way.
   Auto hands control back to the SMC; SetManual forces a duty (safety floor). */
int         Sys_FanAuto(void);
int         Sys_FanSetManual(int pct);

/* Power control via the SMC PIC (reliable on modded boxes, unlike the firmware
   HALT/REBOOT paths). These do not return on real hardware -- the SMC cuts power
   or resets shortly after the call. Reset = warm reboot, PowerCycle = cold off/on,
   PowerOff = shutdown. */
void        Sys_Reset(void);
void        Sys_PowerCycle(void);
void        Sys_PowerOff(void);

int         Sys_RamMB(void);          /* 64 or 128 (detected)            */
int         Sys_RamFreeMB(void);      /* free physical RAM right now, MB  */

/* System clock (local time). Get reads the current local time; Set applies it
   via NtSetSystemTime (returns 1 on success, 0 if the date was invalid or the
   call was refused). Runtime only -- never touches the EEPROM timezone bias. */
typedef struct {
    int year, mon, day, hour, min, sec, dow;
} SysClock;
void        Sys_GetClock(SysClock* c);
int         Sys_SetClock(const SysClock* c);
int         Sys_SetClockDirect(const SysClock* c);   /* set fields as-is (NTP; no EEPROM-TZ reconvert) */

const char* Sys_XboxRevision(void);   /* "1.2 - 1.5 (Focus)" etc.        */
DWORD       Sys_CpuMHz(void);         /* measured CPU clock (OC-aware)    */
DWORD       Sys_GpuMHz(void);         /* measured NV2A GPU clock          */

#endif /* DD_SYSINFO_H */