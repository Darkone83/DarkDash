/*---------------------------------------------------------------------------
    dd_xbe.h -- read metadata out of an Xbox XBE.

    A2: certificate title name + title ID. Only the header region is read
    (a few KB), never the whole executable. A3 will extend this module to
    locate the $$XTIMAGE section and decode its XPR0 into a D3D texture.

    Verified XBE layout (from the ScorchedEarthXB teardown):
      file 0x000  magic "XBEH"
      file 0x104  dwBaseAddr
      file 0x118  dwCertificateAddr (VA; file offset = VA - base)
      cert +0x08  dwTitleId
      cert +0x0C  wszTitleName  (80 bytes, UTF-16LE, <=40 chars)
      cert +0xAC  dwVersion
---------------------------------------------------------------------------*/
#ifndef DD_XBE_H
#define DD_XBE_H

#include <xtl.h>
#include <d3d8.h>
#include "dd_texture.h"

/* Reads the cert title into nameOut (ASCII, NUL-terminated) and the title id.
   Returns 1 on success, 0 if the file isn't a readable XBE. */
int Xbe_ReadTitle(const char* xbePath, char* nameOut, int nameCap, unsigned* titleIdOut);

/* Locates the embedded $$XTIMAGE section, decodes its XPR0 (128x128 swizzled
   X8R8G8B8 / A8R8G8B8, or DXT1) into a D3D texture stored in 'out'.
   Returns 1 on success, 0 if absent/unsupported. Caller frees via Texture_Release. */
int Xbe_LoadTitleImage(IDirect3DDevice8* dev, const char* xbePath, Texture* out);

#endif /* DD_XBE_H */