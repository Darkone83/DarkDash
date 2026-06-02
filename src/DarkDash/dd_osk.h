/*---------------------------------------------------------------------------
    dd_osk.h -- controller on-screen keyboard overlay (adapted from SceneChat).

    Two layouts:
        OSK_TEXT    -- full QWERTY (lower / upper / symbols, X cycles sets)
        OSK_NUMERIC -- 10-key pad for numbers + '.' (IP octets, etc.)

    Usage (overlay on top of whatever screen is active):
        Osk_Open(OSK_NUMERIC, "192", 15);
        ... each frame: int r = Osk_Update(pressed);
            r == 0 still open, 1 confirmed (text in buffer), -1 cancelled
        ... in render pass, after the screen: Osk_Draw(dev);
        Osk_GetText(buf, sizeof buf);

    Controller:
        D-pad        move cursor
        A / LTrigger select key
        B            backspace
        X            cycle key set (text mode only)
        Y            space (text mode only)
        Start        confirm     Back  cancel
---------------------------------------------------------------------------*/
#ifndef DD_OSK_H
#define DD_OSK_H

#include <xtl.h>
#include <d3d8.h>

#define OSK_MAX_LEN 128

enum { OSK_TEXT = 0, OSK_NUMERIC = 1 };

void Osk_Open(int mode, const char* initial, int maxLen);
void Osk_Close(void);
int  Osk_IsOpen(void);
int  Osk_Update(WORD pressed);   /* 0 open, 1 confirm, -1 cancel */
void Osk_Draw(IDirect3DDevice8* d);
void Osk_GetText(char* buf, int buflen);

#endif /* DD_OSK_H */