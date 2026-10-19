# DarkDash

<div align=center>

<img src="https://github.com/Darkone83/DarkDash/blob/main/img/dash.png" width=400><img src="https://github.com/Darkone83/DarkDash/blob/main/img/apps.png" width=400>

</div>

<div align=center>

<img src="https://github.com/Darkone83/DarkDash/blob/main/img/Darkone83.png">

</div>

A lightweight, native dashboard for the original Xbox.

**Current release: 0.1.7**

DarkDash is a clean, themeable replacement dashboard for the OG Xbox. It boots
fast, stays out of your way, and lets you launch your games, apps, emulators,
and homebrew from one place. It is built natively (no script engine sitting on
top), so it is light on resources and quick to navigate — and has a low ram
footprint, leaving plenty of headroom on a stock console.

## Features

- **Launch everything** — separate sections for Applications, Games, Homebrew,
  and Emulators. DarkDash scans your drives, lists what it finds, and lets you
  launch with a button press. If your titles live somewhere off the beaten path
  (an extra drive bay or an unusual layout), you can add your own scan folders
  per section — just browse to the folder and pick it, no typing required. A
  quick **refresh** rescans without leaving the menu, handy right after copying
  new titles over FTP.
- **Hide what you don't use** — press **BLACK** on any title to hide it from its
  list, handy for tidying duplicates or tucking away setup tools you rarely
  touch. Press **START** to toggle a *show-hidden* view where hidden titles
  reappear (marked as hidden) so you can bring them back. Your hidden list is
  saved across reboots.
- **Cover art & titles** — if a title folder has a `_resources` pack, DarkDash
  uses its cover art (PNG or JPG) and game title, shown as a floating hologram on
  the pedestal. If not, it falls back to the title image baked into the game
  (shown on a rotating cube), and finally to a generic placeholder so there is
  always something on the pedestal.
- **Title info** — in any title list, press **WHITE** on a game that has a
  `_resources` pack to pop up an info card: front/back cover art (flip with the
  **D-Pad left/right**) alongside the title, developer, publisher, genre, rating,
  and a scrolling description. Games without a pack simply don't show the prompt.
- **Recently launched** — press **Y** on the main menu for a quick list of the
  last few titles you launched, and jump straight back into one.
- **Jukebox** — press **L3** on the main menu to browse and play MP3 music,
  including tracks in nested folders. Choose a music-library root, create and
  manage playlists, add tracks or folders, shuffle playback, and skip between
  songs. ID3 metadata and embedded cover art appear when available. The
  Jukebox uses DarkDash's existing audio engine and saves its preferences.
- **Save Manager** — browse your game saves by title, see the cover art, and
  copy, move, or delete a game's saves — including to and from memory units.
- **Built-in file manager** — copy, move, rename, and delete files across your
  hard drives and memory units, with a simple two-pane layout. Large copies and
  moves run in the background with a progress bar and can be cancelled.
- **Dual-HDD support** — when CerBIOS is configured for two hard drives,
  accessible secondary-drive partitions are available in the file manager,
  launcher folder picker, game scan paths, and FTP. Common secondary partitions
  use `N:` (HDD1 E), `O:` (HDD1 F), `P:` (HDD1 G), and `R:` (HDD1 C).
  Missing or inaccessible secondary partitions are skipped; single-drive
  installations continue using their normal drive letters.
- **Insert and play** — pop in a game disc and a prompt appears in the top-right
  corner. Press **START** to play it. The disc also shows up in the file manager
  as a drive, so you can browse it like any other.
- **Power menu** — tap **WHITE** on the main menu to restart the dashboard,
  reboot the console, or shut it down.
- **Screensaver** — after a configurable idle time, DarkDash drifts a slow
  showpiece of your cover art across the screen with a rainbow light beam. Any
  button press brings you straight back. Set the timeout (or turn it off) in
  **Settings → Video**.
- **Visual effects** — optional music-reactive plasma, frame arcs, scanlines,
  selection effects, and idle motion, plus theme-aware ambient lighting,
  reflections, depth, and softer alpha-shaped shadows across the interface.
  In **Settings → Video → Effects**, adjust Lighting, Shadows, Reflections,
  Atmosphere, and Parallax independently from 0–200%, or use **Reset Visuals**
  to restore the recommended strengths. These settings scale each theme's
  appearance without replacing its colors and artwork.
- **Sound** — DarkDash has background music and menu sound effects. Pick your
  own music track — or choose **None** for silence, or **Shuffle** to play a
  random track and roll to another each time one finishes — and set the volume
  in **Settings → Audio**.
- **Themes** — DarkDash is fully reskinnable. Swap colors, backgrounds, menu
  icons, panel artwork, and material and lighting styles with drop-in theme
  folders. Themes support liquid, hologram, and metal 3D-stage lighting profiles
  and authored interface frames, with sensible fallbacks for optional assets.
  The existing music-reactive plasma behavior is preserved.
- **Custom fonts** — replace the built-in font with your own. Tall or chunky
  fonts are scaled to fit the on-screen panels automatically, so a custom font
  won't spill out of the menus.
- **Internet time** — DarkDash can set the clock from the internet (NTP). Turn
  it on and pick your time zone in **Settings → Clock**; it syncs on boot and
  on demand.
- **Network** — DHCP works out of the box. If you need it, set a static IP, or
  keep DHCP with your own DNS servers, in **Settings → Network**.
- **FTP** — turn on the built-in FTP server to move files to and from your Xbox
  over the network. A dedicated transfer worker and buffered uploads help keep
  transfers independent of the TV interface. HDD1 uses the same `N:`, `O:`,
  `P:`, and `R:` letters as the file manager; older `/HDD1-*` FTP paths are
  still accepted.
- **Accessories** — DarkDash can control DarkoneCustoms hardware add-ons from
  **Settings → Accessories**: a front-panel LCD (which can show the current
  track while Shuffle is playing), the Type-D info module, and the
  XBOX-RGB and OXFP lighting controllers. Connected devices are selectable; the
  rest stay greyed out until they're found. X-View USB color-display support
  is also available, along with Type-D / Type-D XL game-cover display support.
- **Fan control** — let the console manage the fan automatically, or set a
  manual fan speed, in **Settings → Fan**.
- **Self-updating** — check for and install updates right from the Settings
  menu, with a live download progress bar, then relaunch into the new build —
  no PC required.

## Controls

### Main menu

| Button | Action |
| --- | --- |
| **D-Pad** | Move around the menu |
| **A** | Select / open a section |
| **B** | Back |
| **Y** | Open the recently-launched list |
| **L3** | Open the Jukebox |
| **WHITE** (tap) | Open the power menu |
| **START** | Launch an inserted game disc |

### Applications / Games / Homebrew / Emulators

| Button | Action |
| --- | --- |
| **D-Pad** | Move through the list |
| **LT / RT** | Page up / down (jump a screenful — handy for big libraries) |
| **A** | Launch the highlighted title |
| **WHITE** | Show title info (cover art + details), if the title has a `_resources` pack |
| **Y** | Add a scan folder for this section (browse and pick) |
| **X** | Refresh — rescan this section in place |
| **BLACK** | Hide / unhide the highlighted title |
| **START** | Toggle the show-hidden view |
| **B** | Back |

### File Manager

| Button | Action |
| --- | --- |
| **D-Pad** | Move around the current pane |
| **A** | Enter a folder / drive |
| **X** | Up one level |
| **Y** | Mark / unmark an item |
| **LT / RT** | Switch panes |
| **BLACK** | Open the operations menu (copy / move / delete / rename / new folder) |
| **WHITE** | Paste into the chosen destination |
| **B** | Back |

### Save Manager

| Button | Action |
| --- | --- |
| **D-Pad** | Move through your games |
| **A** | Open actions (copy / move / delete this game's saves) |
| **B** | Exit |

### Jukebox

Browse MP3 folders and playlists using the on-screen button hints. The
Jukebox supports playback controls, shuffle, playlist editing, and a
configurable music root; the available actions vary between its browser,
playlist, and Now Playing views.

## Installing

1. Copy the DarkDash folder to your Xbox (for example over FTP or with a USB
   tool). Keep DarkDash's files together in one folder.
2. Point your softmod / BIOS dashboard path at DarkDash's `default.xbe`, or
   launch it like any other homebrew.
3. That's it — DarkDash will scan your drives on boot.

DarkDash finds its own install folder automatically, so it works the same
whether you launch it as a regular homebrew title or set it as your dashboard.

Optional folders DarkDash looks for in its own folder:

- `themes\` — drop-in theme folders (see the tools folder for a theme builder).
- `fonts\` — custom fonts (see the tools folder for a font maker).
- `data\` — saved settings and a few extras.

## Themes & Fonts

DarkDash ships with a default look, but you can fully reskin it and swap the
font. Both are drop-in: build a theme folder or a font file, copy it to your
Xbox, and pick it in **Settings**.

The `tools` folder includes **Theme Studio**, a PySide6 desktop app for
editing and building themes, previewing artwork, managing the named image
slots, and setting colors and 3D lighting. It can open existing theme folders
without discarding unrelated settings. The **Font Maker** creates `.ddf`
fonts from TTF/OTF files. See `tools/README.md` for dependencies and guides.

## Resource packs and launcher artwork

DarkDash supports game artwork supplied by XBMC4Gamers-style `_resources`
folders. The launcher prioritizes existing cover/case resources and can show
`_resources\artwork\fanart-blur.jpg` (or `fanart.jpg`) as a subdued
background behind the title list. Backgrounds crossfade when changing titles;
when fanart is absent, the selected DarkDash theme background remains in use.

Game discovery and launch behavior do not depend on fanart or a resource pack.

## Updating

Open **Settings → Update**. DarkDash checks the server, and if there's a newer
version it'll download it with an on-screen progress bar, install it, and
relaunch into the new build. If nothing's newer, it just says so.

## A note on game discs

When you insert a disc, DarkDash mounts it to a separate drive letter so the
dashboard keeps working normally, and shows a "press START to play" prompt if
it's an Xbox game. It also appears in the file manager as a drive (labelled
**D:**) so you can browse it. It does not auto-launch — you stay in control.

## Release notes

See the project changelog for changes introduced in **0.1.7**, including
Jukebox, dual-HDD, launcher fanart, FTP updates, adjustable effects, and
theme tooling.

## Credits

- **Darkone83** — design, code, artwork

DarkDash is homebrew for original Xbox hardware. Use it on consoles you own and
have legally modified.
