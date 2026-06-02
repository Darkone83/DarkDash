# DarkDash Tools

Two small desktop apps for making DarkDash content:

- **Theme Builder** (`theme_builder.py`) — build and validate a theme.
- **Font Maker** (`ddf_encoder.py`) — turn a TTF/OTF font into a DarkDash font.

Both are Python apps with a simple window-based interface.

## Requirements

- **Python 3** (with Tkinter, which ships with most Python installs).
- **Pillow** — the image library. Install it with:

  ```
  pip install pillow
  ```

If Pillow is missing, the apps will tell you when you launch them.

To run either tool, double-click it (if `.py`/`.pyw` files open with Python on
your system) or run it from a terminal:

```
python theme_builder.py
python ddf_encoder.py
```

---

# Theme Builder

A DarkDash theme is just a folder. It contains a `theme.ini` file (the settings)
and an `assets` folder (the artwork). The Theme Builder helps you set the colors
and write a correct `theme.ini`, and it checks that your artwork is the right
size.

## How to use it

1. Make a folder for your theme, e.g. `MyTheme`, with an `assets` subfolder
   inside it.
2. Put your artwork PNGs in `assets` (see the sizes below).
3. Launch the Theme Builder and **Choose Theme Folder** — point it at your
   `MyTheme` folder.
4. Set the theme **name**, **author**, **palette** colors, the **glow**, and
   (optionally) a painted **background**.
5. Click **Validate** to check your artwork sizes, then **Write theme.ini**.
6. Copy the whole `MyTheme` folder to your Xbox under `themes\`, then pick it in
   **Settings → Theme**.

## Theme folder layout

```
MyTheme\
    theme.ini
    assets\
        bar_header.png
        bar_footer.png
        frame_menu_v.png
        orb_hero.png
        platform_round.png
        overlay_selection_glow.png
        bg.png              (optional)
```

## Artwork sizes

All artwork is **PNG** with transparency. Use these exact pixel sizes:

| File | Size (W x H) | What it is |
| --- | --- | --- |
| `bar_header.png` | **300 x 40** | The top status bar frame |
| `bar_footer.png` | **624 x 32** | The bottom button-hint bar |
| `frame_menu_v.png` | **272 x 384** | The tall menu/list panel frame |
| `orb_hero.png` | **272 x 234** | The centerpiece "orb" on the main menu |
| `platform_round.png` | **256 x 120** | The pedestal base under the orb |
| `overlay_selection_glow.png` | **220 x 40** | The glow behind the selected row |
| `bg.png` | **640 x 480** | Optional full-screen painted background |

Tips:
- Keep transparency where you want the background to show through (the frames
  and the orb are meant to sit over the dashboard's glow).
- `bg.png` is optional. Include it only if you want a full painted background;
  leave it out to keep the default animated glow.
- Artwork only — the Theme Builder does not change the layout, just the look.

## Colors (the palette)

Colors are entered as 6-digit hex (RRGGBB), the same way you'd write a web
color. The palette keys:

| Key | What it tints |
| --- | --- |
| `accent` | Main highlight color |
| `accent_dim` | A darker version of the accent |
| `glow` | Selected-item glow / highlights |
| `text` | Normal text |
| `text_dim` | Secondary / faded text |
| `bg` | Base background color |

## Glow

The **glow** block controls the selection glow:

- **Enabled** — on or off.
- **Color** — the glow color (RRGGBB). This can be different from the palette.
- **Intensity** — 0 to 100, how strong the glow is.

## Background

- **None** — use the dashboard's default animated glow.
- **Static** — use your `bg.png` as a full-screen painted background.

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