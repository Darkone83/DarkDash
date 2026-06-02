/*---------------------------------------------------------------------------
    dd_swing.h -- "door swing" transition for the menu console.

    A cheap, high-personality screen/menu transition: the menu frame swings
    out around its right edge (hinge) until it's edge-on and invisible, the
    caller swaps content at that hidden midpoint, then it swings back in with
    the new menu. The pedestal/orb glitch keeps running independently.

    To keep the text on the surface, the whole console is captured to an
    offscreen render target, then that texture is mapped onto the swinging
    quad -- so the frame, rows and highlight all turn together.

    Usage from the render loop:
        Swing_Start();                       // kick off (on screen change)
        ... each frame:
        Swing_Update(dtMs);
        if (Swing_TookMidpoint()) { swap to the new menu/selection; }
        // when Swing_Active(): bracket the console draw with capture, then
        // draw the swinging quad:
        if (Swing_CaptureBegin()) {
            ...draw console normally...
            Swing_CaptureEnd();
            Swing_DrawCaptured(x, y, w, h);
        }
---------------------------------------------------------------------------*/
#ifndef DD_SWING_H
#define DD_SWING_H

#include <xtl.h>
#include "dd_texture.h"

void  Swing_Start(void);            /* begin a swing-out -> swap -> swing-in */
void  Swing_StartIn(void);          /* swing IN only (menu arriving, on return) */
int   Swing_Active(void);           /* 1 while animating                     */
void  Swing_Update(DWORD dtMs);     /* advance the animation                 */
int   Swing_TookMidpoint(void);     /* true for ONE frame at edge-on (swap!) */
int   Swing_ContentAlpha(void);     /* legacy; 255 with RTT (content baked)  */

/* The door (frame+text) is turning only in the middle of the sequence; at each
   end there's a rect-fade phase where the console is flat. */
int   Swing_Doorturning(void);      /* 1 while the panel is actually rotating */

/* Selection-rect alpha 0..255: fades out before the swing, hidden during it,
   fades back in after the door closes. Drawn separately from the swung
   texture so the highlight never has to be transformed. */
int   Swing_RectAlpha(void);

/* RTT capture: bracket the console's draw calls with these so the console
   renders into the swing's offscreen target instead of the backbuffer.
   Begin returns 1 if capture is active (draw the console), 0 if not (draw
   normally). End restores the backbuffer. */
int   Swing_CaptureBegin(float vx, float vy, float vw, float vh);
void  Swing_CaptureEnd(void);

/* Draw the captured console as the swinging 3D quad (right-edge hinge).
   vx,vy,vw,vh = the console's flat rect in virtual 640x480 space. */
void  Swing_DrawCaptured(float vx, float vy, float vw, float vh);

#endif /* DD_SWING_H */