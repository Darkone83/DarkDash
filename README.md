# DarkDash

<div align=center>

<img src="https://github.com/Darkone83/DarkDash/blob/main/img/dash.png" width=400><img src="https://github.com/Darkone83/DarkDash/blob/main/img/apps.png" width=400>

</div>

<div align=center>

<img src="https://github.com/Darkone83/DarkDash/blob/main/img/Darkone83.png">

</div>

A lightweight, native dashboard for the original Xbox.

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
- **Save Manager** — browse your game saves by title, see the cover art, and
  copy, move, or delete a game's saves — including to and from memory units.
- **Built-in file manager** — copy, move, rename, and delete files across your
  hard drive and memory units, with a simple two-pane layout. Large copies and
  moves run in the background with a progress bar and can be cancelled.
- **Insert and play** — pop in a game disc and a prompt appears in the top-right
  corner. Press **START** to play it. The disc also shows up in the file manager
  as a drive, so you can browse it like any other.
- **Power menu** — tap **WHITE** on the main menu to restart the dashboard,
  reboot the console, or shut it down.
- **Screensaver** — after a configurable idle time, DarkDash drifts a slow
  showpiece of your cover art across the screen with a rainbow light beam. Any
  button press brings you straight back. Set the timeout (or turn it off) in
  **Settings → Video**.
- **Sound** — DarkDash has background music and menu sound effects. Pick your
  own music track and set the volume in **Settings → Audio**.
- **Themes** — DarkDash is fully reskinnable. Swap colors, the glow, the
  background, the on-screen artwork, and the menu icons with drop-in theme
  folders. A theme can override as much or as little as it likes — anything it
  doesn't include falls back to the default look.
- **Custom fonts** — replace the built-in font with your own. Tall or chunky
  fonts are scaled to fit the on-screen panels automatically, so a custom font
  won't spill out of the menus.
- **Internet time** — DarkDash can set the clock from the internet (NTP). Turn
  it on and pick your time zone in **Settings → Clock**; it syncs on boot and
  on demand.
- **Network** — DHCP works out of the box. If you need it, set a static IP, or
  keep DHCP with your own DNS servers, in **Settings → Network**.
- **FTP** — turn on the built-in FTP server to move files to and from your Xbox
  over the network.
- **Accessories** — DarkDash can control DarkoneCustoms hardware add-ons from
  **Settings → Accessories**: a front-panel LCD, the Type-D info module, and the
  XBOX-RGB and OXFP lighting controllers. Connected devices are selectable; the
  rest stay greyed out until they're found.
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

The `tools` folder has two small desktop apps to help you make these — see the
README in that folder for how to use them, including the exact artwork sizes a
theme needs.

## Updating

Open **Settings → Update**. DarkDash checks the server, and if there's a newer
version it'll download it with an on-screen progress bar, install it, and
relaunch into the new build. If nothing's newer, it just says so.

## A note on game discs

When you insert a disc, DarkDash mounts it to a separate drive letter so the
dashboard keeps working normally, and shows a "press START to play" prompt if
it's an Xbox game. It also appears in the file manager as a drive (labelled
**D:**) so you can browse it. It does not auto-launch — you stay in control.

## Credits

- **Darkone83** — design, code, artwork

DarkDash is homebrew for original Xbox hardware. Use it on consoles you own and
have legally modified.