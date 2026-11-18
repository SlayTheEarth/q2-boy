# Q2 GBA

A native Game Boy, Game Boy Color, and Game Boy Advance emulator frontend
for the Shanling Q2 running Q2-Pod. It installs under **Library → Games → GBA**.
The ROM picker runs `.gb` and `.gbc` with [Gambatte](https://github.com/libretro/gambatte)
and `.gba` with [gpSP](https://github.com/libretro/gpsp) automatically.
All three share the same controls, menus, display options, save slots, and
2×/3× fast-forward options. No ROMs or original Nintendo BIOS are shipped.

The standard Q2-Pod 1.8.8 card-game launcher is sufficient for emulation,
controls, game audio, save states, and fast-forward. Background Q2 music
and access to the firmware's native Now Playing screen use the bridge in
[firmware](firmware/). Upgrading to 0.2.0 requires only the SD-card files.

## Install on the Q2

1. Download `Q2_GBA_v0.2.0.zip` from [Releases](https://github.com/SlayTheEarth/q2-gba/releases/latest).
   Extract it and copy the packaged `.gba` directory to the root of the SD
   card. Enable hidden files if your file manager hides the directory.
2. Copy your `.gb`, `.gbc`, and `.gba` ROM files into `GBA/ROMs` on the same card.
3. Open **Library → Games → GBA**. Upgrading only requires replacing the
   two existing app files and adding `gambatte_libretro.so` in `.gba`. Keep the
   existing `GBA` folder with your ROMs, saves, save states, and settings.

The ZIP's `Source` folder is for rebuilding the app. Only `.gba` goes on
the SD card when installing the app.

## Controls

By default, the Q2's Return, Play/Pause, Previous, and Next buttons act as
Up, Down, Left, and Right during play. Centre acts as A. B, L, R, Start,
Select, and Menu are visible touchscreen overlays.

- **Menu → Display and controls → Physical buttons** assigns each of the
  five Q2 buttons to a GBA action. Remaps apply during gameplay. Centre
  still confirms and Return still goes back in menus.
- **Touch buttons** toggles individual overlays. It includes every GBA
  button, plus A+B and shortcuts for fast-forward and quick save/load.
- **Move, resize, or fade buttons** edits the overlays. Choose an overlay
  and use **Assign action** to remap a gameplay overlay. Menu remains
  available so there's always a way back.
- **Reset controls** restores the default layout and assignments.

## Development

See [building and integration](docs/BUILD.md). The frontend is licensed
under GPL-2.0-or-later, with the combined gpSP build distributed under GPL
version 2. The separately loaded Gambatte core is GPL-2.0-only.
The optional Q2-Pod changes are GPL-3.0-only. The font has its
own [license and attribution](src/FONT-LICENSE).
