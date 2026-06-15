#ifndef DD_CUSTOMADD_H
#define DD_CUSTOMADD_H
/*---------------------------------------------------------------------------
    dd_customadd.h -- "Add custom category" wizard overlay.

    Sequences two overlays you already have: dd_browse to pick the folder, then
    dd_osk to name it, then Custom_Add() (which creates or appends custom.dat).
    Self-contained: it drives Browse/Osk itself and draws them, so a host just
    needs to pump CustomAdd_Update() while open and call CustomAdd_Draw().

    The host MUST check CustomAdd_IsOpen() before any of its own Browse/Osk
    handling so the picker confirms aren't consumed by something else.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    void CustomAdd_Open(void);
    int  CustomAdd_IsOpen(void);
    int  CustomAdd_Update(WORD pressed);   /* 0 open, 1 added (reload cats), -1 cancel */
    void CustomAdd_Draw(IDirect3DDevice8* d);

#ifdef __cplusplus
}
#endif
#endif /* DD_CUSTOMADD_H */