/*---------------------------------------------------------------------------
    dd_pedestal.h -- the launcher's left-hand showpiece.

    Draws a green light blooming up from the pedestal, and (if an image is
    given) the title texture mapped onto a slowly spinning 3D cube sitting in
    its own viewport above the platform. 'art' / 'glow' may be NULL.
---------------------------------------------------------------------------*/
#ifndef DD_PEDESTAL_H
#define DD_PEDESTAL_H

#include <xtl.h>
#include "dd_texture.h"

void Pedestal_Draw(const Texture* art, const Texture* glow,
    DWORD ms, int ar, int ag, int ab);

/* flat billboard quad, gently spinning, opaque -- the Settings category icon. */
void Pedestal_DrawFlat(const Texture* icon,
    DWORD ms, int ar, int ag, int ab);

/* flat quad rendered as a translucent, flickering, swaying hologram -- the
   launcher's _resources case art. */
void Pedestal_DrawHologram(const Texture* icon,
    DWORD ms, int ar, int ag, int ab);

#endif /* DD_PEDESTAL_H */