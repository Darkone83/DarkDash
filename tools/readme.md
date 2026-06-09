# DarkDash Tools

Two small desktop apps for making DarkDash content:

- **Theme Builder** (`theme_builder.py`) — build, theme, and validate a complete
  theme folder from start to finish.
- **Font Maker** (`ddf_encoder.py`) — turn a TTF/OTF font into a DarkDash font.

Both are Python apps with a simple window-based interface.

## Requirements

- **Python 3** (with Tkinter, which ships with most Python installs).
- **Pillow** — the image library. Install it with:

  ```
  pip install pillow
  ```

The Theme Builder uses Pillow to convert your art to PNG, resize it, and check
sizes. Without Pillow it can still copy art that is *already* PNG, but it can't
convert other formats or resize. The Font Maker needs Pillow to rasterize the
font. If Pillow is missing, the apps will tell you when you launch them.

To run either tool, double-click it (if `.py`/`.pyw` files open with Python on
your system) or run it from a terminal:

```
python theme_builder.py
python ddf_encoder.py
```

---

# Theme Builder

A DarkDash theme is just a folder: a `theme.ini` file (the settings) and an
`assets` folder (the artwork). The Theme Builder creates that whole structure for
you — pick where to build it and a name, assign an image to each art slot, set
the colors, glow, and background, and it writes everything out correctly, ready
to copy to your Xbox.

You don't have to make folders or pre-name files by hand anymore. Point the
builder at the images you've got — in almost any common format — and it converts,
renames, and (optionally) resizes them into the right slots.

## How to use it

1. Launch the Theme Builder.
2. **Build Location...** — pick the folder you want the theme built *inside* (for
   example your Desktop). The builder creates a new sub-folder there.
3. Enter a **Name** (and optional **Author**). The builder shows the exact folder
   it will create as you type.
4. For each art **slot**, click **Choose...** and pick your image. PNG, JPG, BMP,
   GIF, and TGA all work — the builder converts to PNG for you.
5. Set the **palette** colors, the **glow**, and (optionally) a painted
   **background**. Each color has a **...** button that opens a color picker, or
   you can type a hex value.
6. Click **Build Theme** (or press **Ctrl+B**). The builder creates the folder,
   imports your art into the correctly-named slots, and writes `theme.ini`. The
   report box shows what it did and flags anything missing.
7. Copy the new theme folder to your Xbox under `themes\`, then pick it in
   **Settings → Theme**.

If you'd rather size your art exactly yourself, leave **Resize imported art** off
and supply images already at the recommended sizes — the builder keeps them
pixel-for-pixel. Turn resizing on to let it scale whatever you give it (handy,
but it may distort the aspect ratio).

## Theme folder layout

The builder produces this for you:

```
MyTheme\
    theme.ini
    assets\
        bar_header.png
        bar_footer.png
        frame_menu_v.png
        orb_hero.png
        platform_round.png
        overlay_selection_glow.png   (optional)
        bg.png                       (optional)
        raw\                         (optional per-game icon overrides)
```

The slot filenames are fixed — DarkDash loads each piece of art by name — which
is why the builder renames whatever you import to the slot name. You don't list
assets in `theme.ini`; the engine finds them by filename.

## Artwork

All artwork ends up as **PNG** with transparency. These are the recommended
pixel sizes:

| File | Size (W x H) | Required? | What it is |
| --- | --- | --- | --- |
| `bar_header.png` | **300 x 40** | yes | The top status bar frame |
| `bar_footer.png` | **624 x 32** | yes | The bottom button-hint bar |
| `frame_menu_v.png` | **272 x 384** | yes | The tall menu/list panel frame |
| `orb_hero.png` | **272 x 234** | yes | The centerpiece "orb" on the main menu |
| `platform_round.png` | **256 x 120** | yes | The pedestal base under the orb |
| `overlay_selection_glow.png` | **220 x 40** | no | The glow behind the selected row |
| `bg.png` | **640 x 480** | no | Optional full-screen painted background |

Tips:
- Keep transparency where you want the background to show through (the frames and
  the orb sit over the dashboard's glow).
- Sizes are *recommended*. Supply art at these sizes and leave resizing off for
  pixel-perfect results, or let the builder resize on import.
- `bg.png` is optional — include it only for a full painted background; leave it
  out to keep the default animated glow.
- Artwork only — the builder changes the look, not the layout.

## Colors (the palette)

Colors are 6-digit hex (RRGGBB), the same as a web color — type them, or use the
**...** color picker next to each one.

| Key | What it tints |
| --- | --- |
| `accent` | Main highlight color |
| `accent_dim` | A darker version of the accent |
| `glow` | Selected-item glow / highlights |
| `text` | Normal text |
| `text_dim` | Secondary / faded text |
| `bg` | Base background color |

## Glow

The **Glow** block controls the selection glow:

- **Enabled** — on or off.
- **Color** — the glow color (RRGGBB), with its own color picker. It can differ
  from the palette.
- **Intensity** — a 0–100 slider for how strong the glow is.

## Background

- **Off** — use the dashboard's default animated glow.
- **Use painted background** — writes your `bg.png` into the theme and uses it as
  a full-screen painted background. (Choosing a `bg` image turns this on for you.)

## Per-game icons

The builder also creates an `assets\raw\` folder for optional per-game icon
overrides. It starts empty; drop icon art in there if you want specific titles to
use custom icons. Leave it empty and DarkDash uses its normal icons.

## Build, save, and validate

- **Build Theme** (Ctrl+B) — create/refresh the folder, import art, write
  `theme.ini`.
- **Save theme.ini** (Ctrl+S) — rewrite just `theme.ini` (palette / glow /
  background) without touching your imported art. Handy for tweaking colors on a
  theme you've already built.
- **Validate** — check a built theme's slots: which are present, whether each is
  the recommended size, and whether any required slot is missing.

You can also validate from a terminal without opening the window:

```
python theme_builder.py path\to\MyTheme
```

---

# Font Maker

The Font Maker turns a normal **TTF or OTF** font into a DarkDash font file
(`.ddf`) that the dashboard can load to replace its built-in font.

## How to use it

1. Launch the Font Maker.
2. **Choose** a `.ttf` or `.otf` font file.
3. **Save** the output `.ddf` file (name it whatever you like).
4. Copy the `.ddf` to your Xbox under `fonts\`, then pick it in
   **Settings → Font**.

That's the whole process — pick a font, save, copy, select.

## Good fonts to try

Clean, readable fonts work best on a TV. Some free options:

- **Rajdhani**, **Orbitron** — techy / sci-fi
- **JetBrains Mono** — crisp monospace
- **Press Start 2P** — retro pixel look

## Notes

- The font is rasterized at a size that looks sharp at 720p; the dashboard
  scales it to fit its layout automatically, so it'll line up with the default
  font's spacing.
- Only the standard printable English characters are included.
- If a font fails to load on the Xbox for any reason, DarkDash just falls back
  to its built-in Default font — you won't get a broken screen.

---

Made by Darkone83.