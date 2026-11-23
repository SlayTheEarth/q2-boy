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

On standard [Q2-Pod 1.9.0](https://github.com/DiamondBond/q2-pod/releases/tag/1.9.0),
Pocket Boy supports Q2 Music and Q2 Now Playing on the first launch. If you
replace the app while Games is open, back out to Library and re-enter Games
so Q2-Pod reads the new launcher settings.

On Q2-Pod 1.8.8, Pocket Boy supports emulation, game audio, save states, and
fast-forward. If prompted on first launch, exit Pocket Boy, back out to
Library, then re-enter Games and launch Pocket Boy again to enable game audio.

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

## Get Games and manage ROMs

On a Q2 connected to Wi-Fi, open the ROM library's **… → Get Games**.
Use **Search** and the on-screen keyboard to find a title or creator across
catalog pages. Open **Filters** to choose the console (GB, GBC or GBA), price,
genre and sorting, then select **Apply**.

Get Games lists projects with verifiable ROM uploads for the chosen console.
A browser-only project or archive whose ROM contents cannot be verified is
omitted. Pocket Boy imports one compatible ROM and saves a matching PNG for
Cover Flow. **View on itch.io** opens the creator page via QR code for paid
games and optional donations.

**Installed** appears when an itch.io download with that project ID is on the
SD card. Deleting the ROM clears the badge even if you keep its saves.

To remove a file, touch and hold its cover or list row, then choose **Delete ROM**. Review the
filename, optionally check **Also delete save data**, then continue to the
separate confirmation. The checkbox starts unchecked. Save RAM, RTC data,
and save states are kept unless you check it. A ROM open in the emulator
must be closed before you can delete it.

## ROM library and artwork

Pocket Boy opens on **Recently Played**, with up to ten games in launch order,
newest first. Press **Return** or swipe up to go to the previous carousel.
Press **Play/Pause** or swipe down to go to the next one. The console carousels
follow Recently Played, and **Favorites** comes last. Navigation wraps and
skips empty console shelves. The centered footer identifies the current shelf.

Tap the heart on the centered cover or beside a title in List view to toggle
its favorite status. A white outline means unpinned and a red heart means pinned.

Turn the wheel, swipe horizontally, or use Previous and Next to browse games
within a shelf. Tap a side cover to select it. Press Centre or tap the center
cover to launch it. Tap the top-right area around **⋯** to show **Get Games**, **Menu**,
and **Sort**. Tap outside the row to hide it. Turning the wheel or pressing a
physical button also hides it and carries out the usual action. Open **Menu** to exit Pocket Boy or resume a loaded game. The game pauses while you browse or use a menu.

Pocket Boy preloads nearby artwork and caches up to 64 resized
covers on the SD card. **Refresh ROMs and covers** clears the cache. Compatible
firmware can dim the screen after 30 seconds of inactivity in menus and
the library. The first touch, button press, or wheel turn wakes the screen.

Add a `.png`, `.jpg`, or `.jpeg` beside the ROM using the same base name,
including spaces and punctuation. For example, place
`Pokemon - Emerald Version.jpg` beside `Pokemon - Emerald Version.gba` in
`q2-boy/ROMs/gba`. If several matching images are present, Pocket Boy tries PNG
first, then JPG and JPEG. PNG supports up to 1024 pixels per side and JPEG
up to 2048, with an 8 MiB file limit. Covers keep their proportions. Games
without readable artwork get a cartridge icon.

Open **Sort** to choose **Title (A–Z)** or **Last played** within each system.
The sort also applies across Favorites. Last played tracks successful launches
and lists unplayed games alphabetically at the end. The same menu offers
**Consoles: List view** and **Refresh ROMs and covers**. Recently Played and
Favorites always use Cover Flow. Pocket Boy remembers favorites, sort, and
view on the SD card.

## Pocket Boy menu

Open **⋯ → Menu** in the library to set the default launch behavior,
change the picture mode and filter, or copy a control layout between systems.
**Launch: Start normally** is the default. **Launch: Resume latest save state**
loads the newest compatible manual state from any of the game's ten slots,
or starts normally when there is no compatible state.

On firmware with card-game Now Playing support, choose **Force close**
to toggle between holding **Return + Centre** for 1.2 seconds and **Off**.
The firmware keeps its normal idle behavior.

## Q2 music and Now Playing

Choose **Game Audio** or **Q2 Music** under **Audio**. Game Audio is the
default. Pocket Boy remembers your choice.

On firmware with [card-game Now Playing support](https://github.com/DiamondBond/q2-pod/pull/463),
Pocket Boy receives all four D-pad buttons while Q2 music plays. From a game, choose **Q2 Now
Playing** in the emulator menu. Q2-Pod takes over the screen and playback
buttons while the game is paused. Press **Return** to go back to the
emulator menu. If you were using Game Audio, Pocket Boy restores it when
you return.

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

Swipe inward from near either side while playing to hide or show the touch
controls. Hidden controls ignore taps. Buttons near either edge
still respond to a tap or a hold.

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

Each console saves its own touch and physical controls. Existing layouts
are inherited on first use. To reuse a layout, open **Menu → Copy control
layout** in the library, choose the console to update, then choose the source.
The copy applies to shared controls and preserves console-specific bindings.

The emulator menu puts the fast-forward toggle beside its speed setting.
**ROM library** pauses the current game. Choose **Resume** to return to
it, or select another cartridge.

## Development

See [building and integration](docs/BUILD.md). The frontend is licensed
under GPL-2.0-or-later, with the combined gpSP build distributed under GPL
version 2. The separately loaded Gambatte and FCEUmm cores are GPL-2.0-only.
The separately loaded Snes9x 2005 core is distributed for non-commercial use
under its own license. See the core notices in the Source folder.
The optional Q2-Pod changes are GPL-3.0-only. The font has its
own [license and attribution](src/FONT-LICENSE).
