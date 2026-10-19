# DarkDash Tools

Two desktop tools help you customize DarkDash:

- **Theme Studio** (`theme_builder.py`) — create themes, edit existing theme folders, preview artwork, choose colors and 3D lighting, and validate assets.
- **Font Maker** (`ddf_encoder.py`) — convert TTF/OTF fonts to the DarkDash `.ddf` format.

## Requirements and launching

- Python 3 on Windows (or another OS with a supported desktop environment).
- **PySide6** for the Theme Studio graphical interface.
- **Pillow** for importing/converting/resizing artwork and inspecting image dimensions; also required by the Font Maker to rasterize fonts.

Install the dependencies from a terminal:

```powershell
py -m pip install PySide6 pillow
```

Launch the tools:

```powershell
py theme_builder.py
py ddf_encoder.py
```

On systems where `python` is the configured command, substitute `python` for `py`. The Theme Studio's GUI needs PySide6; Tkinter is no longer used by that app. Without Pillow, the builder can only copy existing PNG files without conversion or dimension checking. The Font Maker still uses its existing interface and workflow.

---

# Theme Studio

Theme Studio has a dark, tabbed PySide6 interface. It creates themes and can **open an existing theme folder** without discarding unrelated theme settings.

## Quick start: create a theme

1. Open `theme_builder.py`, click **New**, then **Build Location** and choose a parent directory (for example, your Desktop).
2. Enter a **Theme name** and optional **Author**. The target path is displayed above the tabs.
3. Under **Artwork & Assets**, select a named artwork slot, choose **Choose Image**, and preview the image. Only import the artwork you want to replace; unassigned slots may use dashboard fallbacks.
4. Under **Colors & Effects**, select palette colors, enable or disable glow, and set its color and intensity.
5. Under **3D Stage**, select the material style and customize lighting, reflections, beam, arc, movement, and pulse values.
6. Click **Build Theme**. Read the report at the bottom, then use **Validate** to inspect the finished folder.
7. Copy the finished theme folder to the dashboard's `themes\` directory on your Xbox, and select it in **Settings → Theme**.

The tool copies chosen artwork into recognized PNG filenames. Enable **Resize imports to reference size** only when desired: resizing may distort the source aspect ratio, and reference dimensions are not strict requirements.

## Edit an existing theme

1. Click **Open Theme** and select the folder containing `theme.ini`.
2. Update colors, lighting, or artwork assignments. Existing images appear in the asset browser and can be previewed without reimporting them.
3. Click **Save Settings** to save INI settings without rebuilding or copying artwork. Use **Build Theme** when you want to import selected images into that folder.
4. Click **Validate** to check assets.

**Existing configuration is preserved.** Known edited keys are updated, but the tool retains other sections, comments, layout definitions, custom keys, and the configured asset directory. You do not need to remove or regenerate earlier theme information to use the new controls. Saving does not automatically upgrade or replace all existing artwork.

## Tabs and controls

### Artwork & Assets

The asset browser organizes named slots into **Icons**, **Decorative frames**, **Stage**, and **Panels and plaques**. Select an entry to preview the artwork and see its actual pixel size. It supports PNG, JPG/JPEG, BMP, GIF, and TGA imports through Pillow, converting selected files to PNG while preserving transparency when available.

- **Choose Image:** assign replacement artwork to the selected slot.
- **Clear Assignment:** clear a pending import for that slot; it does **not** delete an image already in the theme folder.
- **Use painted background when available:** enable the optional background image in the theme.
- **Resize imports:** optionally scale imported files to their reference dimensions. Leave it off for manually prepared art.

The preview shows the image itself; **it is not a live emulation of DarkDash's Xbox renderer** or its final layers, lighting, animation, and perspective.

### Colors & Effects

Use six-digit hexadecimal (`RRGGBB`) values or the **Pick** buttons:

| Color | Purpose |
| --- | --- |
| Accent | Primary interface highlight |
| Accent Dim | Reduced or unfocused accent |
| Glow | Theme highlight/glow tint |
| Text | Primary text |
| Text Dim | Secondary text |
| BG | Base background color |

The **Glow** controls also provide an enable toggle, separate glow color, and intensity from 0–100.

### 3D Stage

Choose a **Material style**:

| Style | Visual character |
| --- | --- |
| Liquid | Animated plasma-like material |
| Hologram | Brighter emission and lighter reflective treatment |
| Metal | Stronger reflections and more restrained emission/motion |

**Lighting colors** cover Ambient light, Main light, Orb emission, Platform beam, and Energy arcs. Leaving an optional color blank allows it to inherit the theme's normal color settings where supported.

**Light, reflection and motion** includes Ambient strength, Main light strength, Emission strength, Reflections, Beam strength, Arc intensity, Motion, Pulse, and Reflection sharpness. Most strength controls range from 0–200; Reflection sharpness ranges from 2–96.

Use **Use Material Defaults** to reset the 3D Stage controls to the chosen material's defaults. This does not erase your theme's other settings. Plasma/music-reactive behavior remains a dashboard feature, not a separate theme-builder plugin.

## Theme folder layout

A normal generated theme resembles:

```text
MyTheme/
  theme.ini
  assets/
    bar_header.png
    bar_footer.png
    frame_menu_v.png
    orb_hero.png
    platform_round.png
    ...other optional named PNG assets...
    bg.png             (optional painted background)
    raw/               (optional per-game icon overrides)
```

The artwork filenames are recognized names; Theme Studio imports artwork into those slots. Themes may also use a custom `asset_dir` already specified in `theme.ini`; the editor respects it. The `raw/` directory is created for optional icon overrides, but new themes do not require contents in it.

## Named artwork reference sizes

The following values reflect the supplied Default theme artwork and are provided as **reference sizes**, not mandatory limits. The dashboard has fallbacks for missing optional named assets.

| Asset filename | Reference W × H |
| --- | ---: |
| `bar_header.png` | 315 × 44 |
| `bar_footer.png` | 462 × 56 |
| `frame_menu_v.png` | 308 × 430 |
| `frame_menu_h.png` | 486 × 189 |
| `frame_menu_h_selected.png` | 491 × 190 |
| `frame_card_blank.png` | 174 × 247 |
| `frame_card_small.png` | 202 × 103 |
| `frame_featured.png` | 449 × 204 |
| `frame_library.png` | 455 × 201 |
| `frame_media.png` | 338 × 187 |
| `frame_network.png` | 388 × 193 |
| `frame_nowplaying.png` | 355 × 142 |
| `frame_recent.png` | 366 × 154 |
| `frame_status.png` | 268 × 185 |
| `frame_storage.png` | 285 × 194 |
| `card_recent.png` | 261 × 186 |
| `orb_hero.png` | 388 × 334 |
| `orb_small.png` | 176 × 184 |
| `orb_glow.png` | 272 × 234 |
| `globe.png` | 256 × 251 |
| `platform_round.png` | 336 × 190 |
| `platform_long.png` | 452 × 144 |
| `tab_h_normal.png` | 323 × 59 |
| `tab_h_selected.png` | 323 × 64 |
| `tab_v_normal.png` | 266 × 279 |
| `tab_v_selected.png` | 266 × 279 |
| `overlay_selection_glow.png` | 423 × 109 |
| `panel_loading.png` | 286 × 94 |
| `plaque_datetime.png` | 178 × 83 |
| `plaque_freespace.png` | 218 × 84 |
| `plaque_info.png` | 239 × 92 |
| `plaque_signedin.png` | 222 × 89 |
| `plaque_toast.png` | 291 × 80 |
| `tile_cover_small_blank.png` | 164 × 247 |
| `tile_cover_medium_blank.png` | 200 × 269 |
| `tile_feature_blank.png` | 354 × 277 |
| `module_media.png` | 243 × 199 |
| `module_status.png` | 232 × 196 |
| `module_storage.png` | 258 × 206 |
| `icon_games.png` | 104 × 115 |
| `icon_apps.png` | 105 × 114 |
| `icon_media.png` | 112 × 113 |
| `icon_network.png` | 113 × 113 |
| `icon_saves.png` | 114 × 114 |
| `icon_settings.png` | 115 × 113 |
| `bg.png` (optional painted background) | 640 × 480 |

When importing transparent frames or glow artwork, use a clean alpha channel. Avoid baking live text, date/time values, or selectable labels into images: DarkDash draws dynamic content itself.

## Build, save, and validation

- **Build Theme:** creates or updates the target folder, imports chosen images, and saves `theme.ini`. Unassigned existing assets are kept.
- **Save Settings:** writes INI edits only; does not copy new images or replace existing assets.
- **Validate:** checks the named asset slots, image readability and reference dimensions where Pillow is available. Missing optional artwork is reported but is not by itself a validation failure.

For command-line asset validation without opening the GUI:

```powershell
py theme_builder.py "C:\Themes\MyTheme"
```

This prints a validation report and returns a failure exit code for a real validation error. It does not need the PySide6 window to be launched.

**Compatibility note:** The theme builder edits the existing `theme.ini` format. New 3D lighting values are optional for older themes. In-game user-adjustable effects percentages are dashboard preferences, not values that Theme Studio needs to overwrite.

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