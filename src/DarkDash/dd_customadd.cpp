/*---------------------------------------------------------------------------
    dd_customadd.cpp -- see dd_customadd.h.

    State machine:
      CA_FOLDER : dd_browse picks the scan folder
      CA_NAME   : dd_osk (text) names the category
    On name-confirm: Custom_Add(name, folder) -> custom.dat created/appended.

    C89 style, file-scope statics.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_customadd.h"
#include "dd_browse.h"
#include "dd_osk.h"
#include "dd_custom.h"

enum { CA_FOLDER = 0, CA_NAME };

static int  s_open = 0;
static int  s_state = CA_FOLDER;
static char s_path[256];

void CustomAdd_Open(void) {
    s_open = 1;
    s_state = CA_FOLDER;
    s_path[0] = 0;
    Browse_Open("Select a folder for the category");
}

static void CustomAdd_Close(void) {
    if (Browse_IsOpen()) Browse_Close();
    if (Osk_IsOpen())    Osk_Close();
    s_open = 0;
    s_state = CA_FOLDER;
}

int CustomAdd_IsOpen(void) { return s_open; }

int CustomAdd_Update(WORD pressed) {
    if (!s_open) return -1;

    if (s_state == CA_FOLDER) {
        int r = Browse_Update(pressed);
        if (r == 1) {
            Browse_GetPath(s_path, sizeof(s_path));
            Osk_Open(OSK_TEXT, "", CUSTOM_NAME_MAX - 1);
            s_state = CA_NAME;
        }
        else if (r == -1) { CustomAdd_Close(); return -1; }
        return 0;
    }

    if (s_state == CA_NAME) {
        int r = Osk_Update(pressed);
        if (r == 1) {
            char name[CUSTOM_NAME_MAX];
            int added = 0;
            Osk_GetText(name, sizeof(name));
            if (name[0] && s_path[0]) added = Custom_Add(name, s_path);
            CustomAdd_Close();
            return added ? 1 : -1;
        }
        else if (r == -1) { CustomAdd_Close(); return -1; }
        return 0;
    }

    return 0;
}

void CustomAdd_Draw(IDirect3DDevice8* d) {
    if (!s_open) return;
    if (s_state == CA_FOLDER)    Browse_Draw(d);
    else if (s_state == CA_NAME) Osk_Draw(d);
}