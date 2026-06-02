# DarkDash

A lightweight, native dashboard for the original Xbox.

DarkDash is a clean, themeable replacement dashboard for the OG Xbox. It boots
fast, stays out of your way, and lets you launch your games, apps, emulators,
and homebrew from one place. It is built natively (no script engine sitting on
top), so it is light on resources and quick to navigate.

Made by Darkone83 / Team Resurgent.

## Features

- **Launch everything** — separate sections for Applications, Games, Homebrew,
  and Emulators. DarkDash scans your drives, lists what it finds, and lets you
  launch with a button press.
- **Cover art & titles** — if a title folder has a `_resources` pack, DarkDash uses its cover art and game title. If
  not, it falls back to the title image baked into the game.
- **Insert and play** — pop in a game disc and a prompt appears in the top-right
  corner. Press **START** to play it.
- **Built-in file manager** — copy, move, rename, and delete files across your
  hard drive and memory units, with a simple two-pane layout.
- **Themes** — DarkDash is fully reskinnable. Swap colors, the glow, the
  background, and all the on-screen artwork with drop-in theme folders.
- **Custom fonts** — replace the built-in font with your own.
- **FTP** — turn on the built-in FTP server to move files to and from your Xbox
  over the network.
- **Self-updating** — check for and install updates right from the Settings
  menu, no PC required.

## Controls

| Button | Action |
| --- | --- |
| **D-Pad** | Move around menus |
| **A** | Select / open / launch |
| **B** | Back |
| **START** | Launch an inserted game disc (from the main menu) |

## Installing

1. Copy the DarkDash folder to your Xbox (for example over FTP or with a USB
   tool).
2. Point your softmod / BIOS dashboard path at DarkDash's `default.xbe`, or
   launch it like any other homebrew.
3. That's it — DarkDash will scan your drives on boot.

Optional folders DarkDash looks for on its own drive:

- `themes\` — drop-in theme folders (see the tools folder for a theme builder).
- `fonts\` — custom `.ddf` fonts (see the tools folder for a font maker).
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
version it'll offer to download and install it for you, then relaunch into the
new build. If nothing's newer, it just says so.

## A note on game discs

When you insert a disc, DarkDash mounts it to a separate drive letter so the
dashboard keeps working normally, and shows a "press START to play" prompt if
it's an Xbox game. It does not auto-launch — you stay in control.

## Credits

- **Darkone83** — design, code, artwork

DarkDash is homebrew for original Xbox hardware. Use it on consoles you own and
have legally modified.