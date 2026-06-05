#pragma once
/*---------------------------------------------------------------------------
    dd_edit.h -- minimal text/config file editor (undocumented).

    A quiet power-user affordance: in the file manager, click the RIGHT STICK
    (R3) on a highlighted .ini / .cfg / .txt file to open it here. Navigate
    lines, press A to pop the themed OSK pre-filled with that line, edit, and
    save. Not in any menu or the README -- discovered, not advertised.

    Byte-perfect save (ported from XbDiag's FileEdit): unchanged lines are
    written back from the original buffer verbatim, INCLUDING their exact
    terminator bytes (CRLF vs LF), so a Windows-style config stays CRLF and a
    Unix one stays LF. Only edited/inserted lines get rewritten. This is why a
    config edit can't silently corrupt line endings.

    Overlay model: Edit_Open(path) -> each frame Edit_Update(pressed) (returns
    1 when it has closed) -> Edit_Draw(d). While the OSK is up it owns input;
    the editor pumps it.
---------------------------------------------------------------------------*/
#ifndef DD_EDIT_H
#define DD_EDIT_H

#include <xtl.h>
#include <d3d8.h>

#ifdef __cplusplus
extern "C" {
#endif

    /* Is this path an editable text file by extension (.ini/.cfg/.txt)? */
    int  Edit_IsEditable(const char* path);

    /* Open the file for editing. 1 on success (loaded + within size cap), 0 if it
       isn't editable, is too large, or can't be read. */
    int  Edit_Open(const char* path);
    void Edit_Close(void);
    int  Edit_IsOpen(void);

    /* Pump input. Returns 0 still open, 1 closed (exited). */
    int  Edit_Update(WORD pressed);
    void Edit_Draw(IDirect3DDevice8* d);

#ifdef __cplusplus
}
#endif
#endif /* DD_EDIT_H */