OG Xbox Isometric Dashboard - DEFAULT THEME (starter)
=====================================================

WHAT THIS IS
------------
A functional starter theme derived from the concept asset sheets.
Its job is to establish the engine contract (folder layout + theme.ini
schema) and give you real, alpha-cut assets to wire up and validate the
renderer against. It is NOT final production art.

INSTALL PATH
------------
Drop the whole folder at:   <xberoot>\themes\og_iso_default\
The active theme is selected in the dashboard's own config (user state),
which simply points at a theme folder by name. Themes stay shareable.

FOLDER LAYOUT
-------------
theme.ini              The declarative contract. Read once at boot.
assets/                Named production assets referenced by theme.ini.
assets/raw/            Every element auto-sliced from the sheets
                       (s<sheet>_<id>.png), in case you want pieces the
                       named set doesn't cover yet.
assets/manifest.json   Map of named asset -> source sheet + dimensions.
fonts/                 (you add) baked bitmap fonts named in [fonts].

LAYERING (painter's order, back -> front)
-----------------------------------------
1. background   palette.bg or a background module
2. stage        iso set-dressing (orb, platform) as 2D sprites
3. chrome       flat front-facing frames (blank panels)
4. content      live text + cover art, drawn FLAT on top (never skewed)
5. overlay      additive selection glow on the focused item

The iso look lives entirely in the pre-rendered stage art, so there is
no runtime 3D camera or projection math. Functional panels are flat and
front-facing, which keeps all text and cover art crisp and readable.

HOW THE ASSETS WERE PREPARED
----------------------------
- Background checkerboard keyed to alpha (slight edge feather).
- Frame/panel interiors flattened to remove baked concept text/values,
  leaving the chrome border = blank panels.
- Each element cut to its own RGBA PNG; key elements named.

HONEST LIMITATIONS (re-author for production)
---------------------------------------------
- Soft-glow elements (overlay_selection_glow, orb glow horns) have rough
  cut edges - keying can't recover true soft alpha from a flat sheet.
  They work as ADDITIVE layers where the fringe matters less, but should
  be re-exported with real soft/premultiplied alpha.
- Frame interiors are flattened to a flat dark tone, not a clean
  re-rendered surface. Fine as blanks; repaint for final polish.
- The orb is currently one baked image (body + glow together). For the
  "animate light, not geometry" approach, re-export it split into a
  static body and a separate additive energy/glow layer so the swirl can
  scroll and the glow can pulse.
- Thin bright edge lines will shimmer on 480i. Thicken key lines in the
  production art and test on a real CRT early.

NEXT STEPS
----------
1. Wire the renderer to theme.ini and confirm the 5-layer composite.
2. Add baked bitmap fonts and point [fonts] at them.
3. Replace assets as you re-author them - the contract stays the same.
