/*---------------------------------------------------------------------------
    dd_eeprom.cpp -- see dd_eeprom.h.

    Audio flags only. Read via XGetAudioFlags(); write via the kernel's
    single-setting nonvolatile API (no raw-EEPROM crypto needed).
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "xboxinternals.h"   /* XC_AUDIO_FLAGS (=0x9) + Ex*NonVolatileSetting */
#include "dd_eeprom.h"

/* XC_AUDIO_FLAGS and ExSaveNonVolatileSetting come from xboxinternals.h.
   The single-index form writes one setting and lets the kernel maintain the
   EEPROM region checksum (no raw-image decrypt/HMAC needed for this field). */
#define DD_REG_DWORD     4

   /* XC_AUDIO_FLAGS_* bit layout (from Xbox.h):
        basic mode  : bit0 = mono, bit1 = surround, 0 = stereo  (low word)
        encoded     : bit16 = AC3, bit17 = DTS                  (high word)   */
#define A_MONO      0x00000001UL
#define A_SURROUND  0x00000002UL
#define A_AC3       0x00010000UL
#define A_DTS       0x00020000UL

void Eeprom_GetAudio(int* mode, int* ac3, int* dts) {
    DWORD f = XGetAudioFlags();
    if (mode) {
        if (f & A_MONO)     *mode = DD_AUDIO_MONO;
        else if (f & A_SURROUND) *mode = DD_AUDIO_SURROUND;
        else                     *mode = DD_AUDIO_STEREO;
    }
    if (ac3) *ac3 = (f & A_AC3) ? 1 : 0;
    if (dts) *dts = (f & A_DTS) ? 1 : 0;
}

int Eeprom_SetAudio(int mode, int ac3, int dts) {
    DWORD flags = 0;
    LONG  r;

    if (mode == DD_AUDIO_MONO)     flags |= A_MONO;
    else if (mode == DD_AUDIO_SURROUND) flags |= A_SURROUND;
    /* stereo = 0, no bit */
    if (ac3) flags |= A_AC3;
    if (dts) flags |= A_DTS;

    /* keep only the valid bits so we never write stray data into the field */
    flags &= (A_MONO | A_SURROUND | A_AC3 | A_DTS);

    r = ExSaveNonVolatileSetting(XC_AUDIO_FLAGS, DD_REG_DWORD, &flags, 4);
    return (r >= 0) ? 1 : 0;
}