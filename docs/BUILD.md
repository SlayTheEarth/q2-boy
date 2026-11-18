# Building and Q2-Pod integration

Use LLVM Clang and LLD, a Shanling Q2 sysroot containing the firmware's
MIPS glibc and ALSA libraries, and matching MIPS Linux/ALSA headers.
The loader is `/lib/ld-linux-mipsn8.so.1`. Debian Buster's MIPS glibc 2.28
headers match the runtime used for testing. The sysroot layout follows
[q2-groovebox](https://github.com/DiamondBond/q2-groovebox).

For the Debian headers, copy `gnu/stubs-o32_hard.h` to
`gnu/stubs-o32_hard_2008.h` under the MIPS multiarch include directory.
Use the Q2 firmware's libraries when linking.
For Gambatte, extract the Debian MIPS little-endian `libstdc++-10-dev`
package to a separate directory and set `CXX_HEADERS` to that directory.
Place the firmware's `libstdc++.so.6` in `$SYSROOT/lib`.

```sh
git clone https://github.com/libretro/gpsp.git deps/gpsp
git -C deps/gpsp checkout 5819380c2ffb0900219d700a382ee68c464ebb99
git -C deps/gpsp apply "$PWD/patches/gpsp-q2.patch"
export SYSROOT=/absolute/path/to/q2-sysroot
Q2_CC="clang --target=mipsel-linux-gnu --sysroot=$SYSROOT -isystem $SYSROOT/usr/include/mipsel-linux-gnu -mabi=32 -mfp64 -mnan=2008 -mabs=2008"
make -C deps/gpsp -j4 platform=mips32 STATIC_LINKING=1 TARGET=gpsp.a \
  CC="$Q2_CC" CXX="$Q2_CC" FRONTEND_SUPPORTS_RGB565=1
mkdir -p dist
cp -a package/Copy-to-SD/. dist/
GPSP="$PWD/deps/gpsp" sh src/build.sh dist/.gba/q2-gba

git clone https://github.com/libretro/gambatte.git deps/gambatte
git -C deps/gambatte checkout 77f2567d718b8973d02caa2daa8dce4953faf811
export CXX_HEADERS=/absolute/path/to/extracted/headers
GAMBATTE="$PWD/deps/gambatte" sh src/build-gambatte.sh dist/.gba/gambatte_libretro.so
```

For an SD release, zip the contents of `dist` at the archive root,
including the hidden `.gba` directory.

## Optional firmware bridge

The frontend can run on standard 1.8.8. That build gives card games
exclusive audio. The [bridge patch](../firmware/q2-pod-gba-audio.patch)
adds a Q2-Pod host protocol for switching audio ownership, native volume
and Now Playing. It also restores the GBA framebuffer and controls on
return from the native screen. The present implementation is scoped to
the `.gba` card app. A reusable opt-in card-game interface would be a
natural upstream adaptation if the Q2-Pod maintainer prefers it.

The patch targets Q2-Pod 1.8.8, commit
`5c44a4d163bc73c9e2c609dc2c80da8589267ada`. Apply it at the checkout root
and use Q2-Pod's normal build with the pinned original Shanling Q2 V1.36 ZIP.
The stand-alone `firmware/repack-q2.py` takes the official 1.8.8
`update.tar`, the patched `demo`, an output archive, and a new scratch
directory. It keeps the release kernel and replaces the player executable.
Firmware images are built separately from the SD application.

## Smoke test

`tests/smoke.s` is a tiny homebrew GBA program and
`python3 tests/make-gb-fixtures.py /path/to/fixtures` generates original
GB and GBC test ROMs. Use
`--smoke /path/to/test.gba /path/to/test-directory /path/to/core-qa.gb /path/to/core-qa.gbc`
to exercise both cores in one process. Only the first ROM and the test
directory are required. The test directory must exist. Tests run against
Q2 firmware libraries in a MIPS emulator. Use the physical Q2 for playback,
screen, and input checks.
