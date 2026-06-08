#ifndef DARKDASH_PLASMA_H
#define DARKDASH_PLASMA_H
/*---------------------------------------------------------------------------
    dd_plasma -- the "flubber": a lit, glossy, organic liquid mass on the
    pedestal, in the spirit of the original Xbox boot blob.

    A UV sphere whose radius is pushed around by several slowly travelling
    lumps plus an overall breathe + pulse, so it reads as a morphing liquid
    mass rather than a rigid ball. Vertex normals are recomputed every frame
    from the deformed surface, and the blob is lit with a real directional
    light + specular (material diffuse = theme base colour), drawn opaque --
    that's what makes it read as a 3D wet mass with a sliding sheen. The
    pedestal light shaft is drawn beneath it.

    Toggled by the DD_FX_PLASMA effect flag -- when off, main.cpp draws the
    normal orb_hero sprite instead.
---------------------------------------------------------------------------*/
#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Animate + draw the plasma orb. vx,vy,vw,vh are the orb's virtual-coord
       rect, used as the 3D sub-viewport. 'accent' is the theme base colour the
       plasma ramp is derived from; 'glow' is currently unused (the accent tone
       is derived from the base). Cheap to call every frame; sets up lazily. */
    void Plasma_Draw(float vx, float vy, float vw, float vh, DWORD accent, DWORD glow);

    /* Reset internal state (tables rebuild on next draw). Safe if never drawn. */
    void Plasma_Release(void);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_PLASMA_H */