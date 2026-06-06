#pragma once
/*---------------------------------------------------------------------------
    dd_typed.h -- Type-D status broadcaster (DarkoneCustoms).

    Type-D is an ESP32 display that listens on UDP 50504 for a small telemetry
    packet and shows it. DarkDash broadcasts that packet so Type-D can display
    what's running plus live fan/temp readings. When enabled, Lcd-style values
    (fan %, CPU temp, ambient/board temp) ride along with the app name; the
    receiver renders whichever it wants.

    Config (enable/disable) lives in the shared dc.dat (DarkoneCustoms config),
    versioned with safe upgrade so an older file isn't wiped.

    Usage:
        TypeD_Init()   once at boot
        TypeD_Tick()   every frame (rate-limited internally)
---------------------------------------------------------------------------*/
#ifndef DD_TYPED_H
#define DD_TYPED_H

#ifdef __cplusplus
extern "C" {
#endif

    void TypeD_Init(void);
    void TypeD_Tick(void);

    int  TypeD_Enabled(void);
    void TypeD_SetEnabled(int on);   /* persists to dc.dat */

#ifdef __cplusplus
}
#endif
#endif /* DD_TYPED_H */