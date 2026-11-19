# q2-boy

<img src="assets/mascot.png" width="220" alt="Pixel-art q2-boy mascot holding a click-wheel player">

A native Game Boy, Game Boy Color, and Game Boy Advance emulator frontend
for the Shanling Q2 running Q2-Pod. It appears as **Pocket Boy** in **Library → Games**.
The ROM picker runs `.gb` and `.gbc` with [Gambatte](https://github.com/libretro/gambatte)
and `.gba` with [gpSP](https://github.com/libretro/gpsp) automatically.
All three share the same controls, menus, display options, save slots, and
2×/3× fast-forward options. No ROMs or original Nintendo BIOS are shipped.

The standard Q2-Pod 1.8.8 card-game launcher is sufficient for emulation,
controls, game audio, save states, and fast-forward. Background Q2 music
and native Now Playing use the [q2-boy firmware bridge](https://github.com/DiamondBond/q2-pod/pull/426).
Firmware with that bridge keeps music already playing when q2-boy opens.

## Install on the Q2

1. Download the latest q2-boy ZIP from [Releases](https://github.com/SlayTheEarth/q2-boy/releases/latest).
   Extract it and copy the packaged `.boy` directory to the root of the SD
   card. Enable hidden files if your file manager hides the directory.
2. Copy your `.gb`, `.gbc`, and `.gba` ROM files into `q2-boy/ROMs` on the same card.
3. Open **Library → Games → Pocket Boy**. On a fresh install the app creates
   the `q2-boy` data folder at first launch.

The ZIP's `Source` folder is for rebuilding the app. Only `.boy` goes on
the SD card when installing the app.

When upgrading, first move the old hidden launcher folder off the card.
On first launch q2-boy moves the previous data folder into `q2-boy` if
that folder does not exist. If both data folders exist, copy any ROMs,
saves, and settings you want to keep into the new folder.

## ROM library and artwork

Pocket Boy opens in **Cover Flow**. Swipe up to advance through the Game Boy,
Game Boy Color, and Game Boy Advance shelves, or swipe down to go back. Empty
systems are skipped. A marker on the right shows which shelf is open. Pocket
Boy opens on the last played system, or the first available system on a fresh
install, starting with Game Boy.

Turn the wheel, swipe horizontally, or use Previous and Next to browse games
within a shelf. Tap a side cover to select it. Press Centre or tap the center
cover to launch it. **Exit** is in the top left and becomes **Resume** while a
game is running.

Add a `.png`, `.jpg`, or `.jpeg` beside the ROM using the same base name,
including spaces and punctuation. For example, place
`Pokemon - Emerald Version.jpg` beside `Pokemon - Emerald Version.gba` in
`q2-boy/ROMs`. If several matching images are present, Pocket Boy tries PNG
first, then JPG and JPEG. PNG supports up to 1024 pixels per side and JPEG
up to 2048, with an 8 MiB file limit. Covers keep their proportions. Games
without readable artwork get a cartridge icon.

Open **Sort** to choose **Title (A–Z)** or **Last played** within each system.
Last played tracks successful launches and lists unplayed games alphabetically
at the end. The same menu offers **List view** and **Refresh ROMs and covers**.
Pocket Boy remembers the sort and view on the SD card.

## Controls

By default, the Q2's Return, Play/Pause, Previous, and Next buttons act as
Up, Down, Left, and Right during play. Centre acts as A. B, L, R, Start,
Select, and Menu are visible touchscreen overlays.
Their backgrounds default to 30% transparency. L and R sit higher and
farther in from the corners. Menu, Select, and Start sit near the bottom.

- **Menu → Display and controls → Physical buttons** assigns each of the
  five Q2 buttons to a cartridge action. Remaps apply during gameplay. Centre
  still confirms and Return still goes back in menus.
- **Touch buttons** toggles individual overlays. It includes every Game Boy Advance
  button, plus A+B and shortcuts for fast-forward and quick save/load.
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
version 2. The separately loaded Gambatte core is GPL-2.0-only.
The optional Q2-Pod changes are GPL-3.0-only. The font has its
own [license and attribution](src/FONT-LICENSE).
