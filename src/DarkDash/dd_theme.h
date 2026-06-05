#ifndef DARKDASH_THEME_H
#define DARKDASH_THEME_H
/*---------------------------------------------------------------------------
    dd_theme -- declarative theme loader for DarkDash.

    Reads <themeRoot>\theme.ini (the versioned contract: [manifest],
    [palette], etc.), exposes palette colours, and lazily loads named assets
    from <themeRoot>\<asset_dir>\<name>.png into a small texture cache.

    Basic bring-up uses palette + named assets; full [region:*] layout
    consumption is a later step.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include "dd_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

    /* Load a theme from its root folder, e.g. "D:\\themes\\default".
       ('D:' is the launched XBE's home directory at runtime.)
       Returns 1 on success, 0 if theme.ini is missing/unreadable. */
    int Theme_Load(const char* themeRoot);
    void Theme_Unload(void);

    /* Palette colour by key (e.g. "accent","bg","glow","text"), as ARGB.
       Returns 'fallback' if the key is absent. */
    DWORD Theme_Color(const char* key, DWORD fallback);

    /* Convenience: clear colour from [palette] bg. */
    DWORD Theme_BG(void);

    /* Lazily load + cache an asset by name (no extension), e.g. "orb_hero".
       Returns NULL if not found. Texture lifetime is owned by the theme. */
    const Texture* Theme_Asset(const char* name);

    /* Resolve a raw asset/icon file (e.g. "s2_020.png") to a full path, preferring
       the ACTIVE theme's assets\raw folder and falling back to the default theme
       if the active theme doesn't ship that file. Writes the chosen path into out.
       Lets themes override the menu icons while keeping default as the safety net. */
    void Theme_ResolveIcon(const char* name, char* out, int cap);

    /*---- theme discovery -------------------------------------------------------
       Scan a themes root (e.g. "D:\\themes") for sub-folders containing a
       theme.ini. "default" is always reported first if present. Names are the
       sub-folder names; build a full root with Theme_RootFor(). */
#define THEME_NAME_MAX  64
    int  Theme_Scan(const char* themesRoot);          /* returns count found */
    int  Theme_Count(void);
    const char* Theme_NameAt(int idx);                /* sub-folder name      */
    /* compose "<themesRoot>\\<name>" into out (for Theme_Load). */
    void Theme_RootFor(const char* themesRoot, const char* name, char* out, int cap);

    /*---- glow (themeable selection / orb additive highlight) -------------------
       From [glow]: enabled (0/1), color (RRGGBB), intensity (0..100). Falls back
       to sensible defaults / palette.glow when absent. */
    int   Theme_GlowEnabled(void);
    DWORD Theme_GlowColor(void);       /* ARGB; defaults to palette.glow */
    int   Theme_GlowIntensity(void);   /* 0..100, default 100            */

    /*---- optional painted background -------------------------------------------
       From [background]: if module=static and image=<file> loads, returns its
       texture (drawn full-screen behind everything); else NULL -> caller uses the
       solid palette.bg + procedural bloom. */
    const Texture* Theme_BackgroundImage(void);

#ifdef __cplusplus
}
#endif
#endif /* DARKDASH_THEME_H */