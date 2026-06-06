/*---------------------------------------------------------------------------
    dd_lcd.h -- physical character-LCD service (US2066 20x4 OLED over SMBus).

    Retooled from XbDiag's LCD driver for DarkDash: runs as a BACKGROUND
    service (Lcd_Tick() pumped from the main loop every frame, regardless of
    which screen is open), pulls live data from DarkDash's own Sys, Net and Ftp
    sources, and rotates a user-chosen set of pages. Config lives in its own
    versioned D:\data\lcd.dat (separate from settings).

    Differences from the XbDiag original:
      - SMBus address is selectable: 0x3C or 0x3D (8-bit 0x78 / 0x7A).
      - No CerBIOS gating/blocking (DarkDash drives the bus directly).
      - Pages are data-driven and toggleable (temps / memory / disk / net /
        FTP transfer / clock), not a fixed diagnostic set.

    Usage:
        Lcd_Init()    once at boot (loads lcd.dat, probes the display, splash)
        Lcd_Tick()    every frame from the main loop
        Lcd_Shutdown() optional, clears the panel on clean exit
---------------------------------------------------------------------------*/
#ifndef DD_LCD_H
#define DD_LCD_H

#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* page-type bits (which info screens rotate); stored in lcd.dat */
    enum {
        LCD_PAGE_TEMPS = 0x01,   /* CPU / board temp + fan %        */
        LCD_PAGE_MEM = 0x02,   /* memory used / total             */
        LCD_PAGE_DISK = 0x04,   /* user partitions free/total      */
        LCD_PAGE_NET = 0x08,   /* IP address / link               */
        LCD_PAGE_FTP = 0x10,   /* live FTP status + transfer       */
        LCD_PAGE_CLOCK = 0x20,   /* date + time                      */
        LCD_PAGE_DISK2 = 0x40    /* disk overflow page -- derived from LCD_PAGE_DISK,
                                    NOT user-toggleable and NOT in LCD_PAGE_ALL;
                                    added to the rotation automatically when more
                                    user partitions are present than fit on one
                                    screen (header + 3 rows). */
    };
#define LCD_PAGE_ALL  (LCD_PAGE_TEMPS|LCD_PAGE_MEM|LCD_PAGE_DISK|LCD_PAGE_NET|LCD_PAGE_FTP|LCD_PAGE_CLOCK)

    /* address choices */
    enum { LCD_ADDR_3C = 0, LCD_ADDR_3D = 1 };

    void Lcd_Init(void);
    void Lcd_Tick(void);
    void Lcd_Shutdown(void);

    int  Lcd_IsPresent(void);          /* 1 if a panel answered at the set address */

    /* ---- config (backed by lcd.dat; setters persist) ----------------------- */
    int  Lcd_Enabled(void);
    void Lcd_SetEnabled(int on);

    int  Lcd_AddrChoice(void);         /* LCD_ADDR_3C / LCD_ADDR_3D               */
    void Lcd_SetAddrChoice(int choice);/* re-probes + re-inits at the new address */

    int  Lcd_Pages(void);              /* page-type bitmask                       */
    void Lcd_SetPages(int mask);
    void Lcd_TogglePage(int bit);

    int  Lcd_IntervalMs(void);         /* page rotation interval                  */
    void Lcd_SetIntervalMs(int ms);

    int  Lcd_Brightness(void);         /* OLED contrast 0..255                    */
    void Lcd_SetBrightness(int v);     /* applies live + persists                 */

    /* Draw a static "Now Playing" screen for the title that's about to launch.
       Called right before the dashboard hands off to a game (after which we lose
       the panel), so it stays frozen on this screen while the game runs. No-op if
       the LCD isn't enabled / present. On return to the dash, Lcd_Tick resumes the
       normal rotating pages (the shadow is invalidated so it redraws fresh). */
    void Lcd_NowPlaying(const char* title);

#ifdef __cplusplus
}
#endif
#endif /* DD_LCD_H */