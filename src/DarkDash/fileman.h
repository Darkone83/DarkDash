/*---------------------------------------------------------------------------
    FileMan.h -- dual-pane FILE MANAGER screen.

    Commander-style layout: two flat panes side by side, source (left) and
    destination (right), each with its own path-header banner. One pane is
    active at a time. Built on dd_fileops for the actual operations.

    Screen contract matches the launcher (enter / update-returns-1-on-exit /
    render) so main.cpp routes it the same way.

    Controls:
      Up/Down  navigate active pane      A      enter folder / run XBE
      LT/RT    switch active pane        X      up a directory
      Y        mark/unmark item          B      cancel / exit (confirm at root)
      Black    operations menu           White  confirm paste (dest-pick mode)
---------------------------------------------------------------------------*/
#ifndef FILEMAN_H
#define FILEMAN_H

#include <xtl.h>

void FileMan_Enter(void);
int  FileMan_Update(WORD pressed, WORD held);  /* returns 1 to exit */
void FileMan_Render(void);

#endif /* FILEMAN_H */