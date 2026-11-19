# q2-boy

<img src="assets/mascot.png" width="220" alt="Pixel-art q2-boy mascot holding a click-wheel player">

A native emulator frontend for the Shanling Q2 running Q2-Pod. It appears
as **Pocket Boy** in **Library → Games**. The ROM picker selects the core automatically.

| System | Folder | ROM extensions | Core |
| --- | --- | --- | --- |
| Game Boy | `gb` | `.gb` | [Gambatte](https://github.com/libretro/gambatte) |
| Game Boy Color | `gbc` | `.gbc` | Gambatte |
| Game Boy Advance | `gba` | `.gba` | [gpSP](https://github.com/libretro/gpsp) |
| NES | `nes` | `.nes` | [FCEUmm](https://github.com/libretro/libretro-fceumm) |
| Super NES | `snes` | `.sfc`, `.smc` | [Snes9x 2005](https://github.com/libretro/snes9x2005) |

All systems share the same menus, display options, save slots, and 2×/3×
fast-forward settings. Actual fast-forward speed depends on the game.
The archive includes no commercial ROMs or Nintendo BIOS files.

The standard Q2-Pod 1.8.8 card-game launcher is sufficient for emulation,
controls, game audio, save states, and fast-forward. Background Q2 music
and native Now Playing use the [q2-boy firmware bridge](https://github.com/DiamondBond/q2-pod/pull/426).
Firmware with that bridge keeps music already playing when q2-boy opens.

## Install on the Q2

1. Download the latest q2-boy ZIP from [Releases](https://github.com/SlayTheEarth/q2-boy/releases/latest).
   Extract it and copy the packaged `.boy` directory to the root of the SD
   card. Enable hidden files if your file manager hides the directory.
2. Open **Library → Games → Pocket Boy** to create the SD card folders.
3. Copy your ROMs into the matching system folder under `q2-boy/ROMs`.
   Save RAM, RTC files, and save states go under the same system name in
   `q2-boy/Saves`.

The ZIP's `Source` folder is for rebuilding the app. Only `.boy` goes on
the SD card when installing the app.

When upgrading, first move the old hidden launcher folder off the card.
On first launch q2-boy moves the previous data folder into `q2-boy` if
that folder does not exist. If both data folders exist, copy any ROMs,
saves, and settings you want to keep into the new folder.

Pocket Boy moves legacy files from the flat `ROMs` folder into their
system folders when it starts. Matching cover art follows the ROM.
Shared covers are copied to each system and the original is kept.
Legacy save files are copied to the system folders with their original
filenames. Existing destination files take priority. If any old files
remain, Pocket Boy can still read them from the flat folders.

## ROM library and artwork

Pocket Boy opens in **Cover Flow**. Press **Return** or swipe up to go to the
previous system. Press **Play/Pause** or swipe down to go to the next system.
The order is Game Boy, Game Boy Color, Game Boy Advance, NES, and SNES.
Navigation wraps at both ends and skips empty systems. A marker on the
right shows the current shelf. Pocket Boy opens on the last played system,
or the first available system on a fresh install, starting with Game Boy.

Turn the wheel, swipe horizontally, or use Previous and Next to browse games
within a shelf. Tap a side cover to select it. Press Centre or tap the center
cover to launch it. **Exit** is in the top left and becomes **Resume** while a
game is running.

Add a `.png`, `.jpg`, or `.jpeg` beside the ROM using the same base name,
including spaces and punctuation. For example, place
`Pokemon - Emerald Version.jpg` beside `Pokemon - Emerald Version.gba` in
`q2-boy/ROMs/gba`. If several matching images are present, Pocket Boy tries PNG
first, then JPG and JPEG. PNG supports up to 1024 pixels per side and JPEG
up to 2048, with an 8 MiB file limit. Covers keep their proportions. Games
without readable artwork get a cartridge icon.

Open **Sort** to choose **Title (A–Z)** or **Last played** within each system.
Last played tracks successful launches and lists unplayed games alphabetically
at the end. The same menu offers **List view** and **Refresh ROMs and covers**.
Pocket Boy remembers the sort and view on the SD card.

## Picture scaling

Open **Menu → Display and controls** while playing. **Picture** cycles through
**Original aspect**, **Crop to fill**, **Stretch to fill**, and **Integer Scale**.
Integer Scale uses the largest whole-number fit with square pixels: 2× for
GB and GBC, and 1× for GBA, NES, and typical SNES games. Higher-resolution
SNES modes that exceed the screen fall back to Original aspect.

**Filter** switches between **Sharp Bilinear** (the default) and **Nearest**.
Sharp Bilinear smooths fractional pixel edges while preserving the larger
picture. Try **Original aspect** with **Sharp Bilinear** for GBA. Integer
multiples stay sharp with either filter.

## Controls

By default, the Q2's Return, Play/Pause, Previous, and Next buttons act as
Up, Down, Left, and Right during play. Centre acts as A. B, L, R, Start,
Select, and Menu are visible touchscreen overlays.
SNES adds X and Y touchscreen buttons on the right. NES hides the default
shoulder buttons. Their backgrounds default to 30% transparency. Menu,
Select, and Start sit near the bottom. NES and SNES use a 4:3 picture
with the default Fit setting.

- **Menu → Display and controls → Physical buttons** assigns each of the
  five Q2 buttons to a cartridge action. Remaps apply during gameplay. Centre
  still confirms and Return still goes back in menus.
- **Touch buttons** toggles individual overlays, including SNES X and Y
  while playing SNES. It also includes A+B and shortcuts for fast-forward
  and quick save/load.
- **Move, resize, or fade buttons** edits the overlays. Choose an overlay
  and use **Assign action** to remap a gameplay overlay. Menu remains
  available so there's always a way back.
- **Reset controls** restores the default layout and assignments.

The emulator menu puts the fast-forward toggle beside its speed setting.
**ROM library** keeps the current cartridge running at normal speed in the
background. Choose **Resume** to return to it, or select another cartridge.

## Development

See [building and integration](docs/BUILD.md). The frontend is licensed
under GPL-2.0-or-later, with the combined gpSP build distributed under GPL
version 2. The separately loaded Gambatte and FCEUmm cores are GPL-2.0-only.
The separately loaded Snes9x 2005 core is distributed for non-commercial use
under its own license. See the core notices in the Source folder.
The optional Q2-Pod changes are GPL-3.0-only. The font has its
own [license and attribution](src/FONT-LICENSE).
