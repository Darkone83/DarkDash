/*---------------------------------------------------------------------------
    Settings.h -- SETTINGS screen.

    Scaffold: the category list is present and navigable, but the rows are
    placeholders -- A does nothing yet. Each category (Network, FTP, Video,
    Audio, Region, Clock, Theme, Font, About) gets built out one at a time.
    Same screen contract as the launcher (enter / update-returns-1-on-B /
    render) so main.cpp routes it the same way.
---------------------------------------------------------------------------*/
#ifndef SETTINGS_H
#define SETTINGS_H

#include <xtl.h>

void Settings_Enter(void);
int  Settings_Update(WORD pressed, WORD held);  /* returns 1 to exit (B) */
void Settings_Render(void);

#endif /* SETTINGS_H */