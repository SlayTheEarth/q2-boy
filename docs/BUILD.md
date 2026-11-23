# Building and Q2-Pod integration

Use LLVM Clang and LLD, a Shanling Q2 sysroot containing the firmware's
MIPS glibc and ALSA libraries, and matching MIPS Linux/ALSA headers.
The loader is `/lib/ld-linux-mipsn8.so.1`. Debian Buster's MIPS glibc 2.28
headers match the runtime used for testing. The sysroot layout follows
[q2-groovebox](https://github.com/DiamondBond/q2-groovebox).

For the Debian headers, copy `gnu/stubs-o32_hard.h` to
`gnu/stubs-o32_hard_2008.h` under the MIPS multiarch include directory.
Use the Q2 firmware's libraries when linking.
The Cover Flow browser loads the firmware's `libpng12.so.0` and
`libjpeg.so.7` at runtime. The JPEG v7-compatible headers in `src/jpeg7`
come from libjpeg-turbo 2.1.5.1. See their included license files.
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
GPSP="$PWD/deps/gpsp" sh src/build.sh dist/.boy/q2-boy

git clone https://github.com/libretro/gambatte.git deps/gambatte
git -C deps/gambatte checkout 77f2567d718b8973d02caa2daa8dce4953faf811
git -C deps/gambatte apply "$PWD/patches/gambatte-q2.patch"
export CXX_HEADERS=/absolute/path/to/extracted/headers
GAMBATTE="$PWD/deps/gambatte" sh src/build-gambatte.sh dist/.boy/gambatte_libretro.so

git clone https://github.com/libretro/libretro-fceumm.git deps/fceumm
git -C deps/fceumm checkout 77468e568d9fe9c48d684ca911f1a3fe7259fdf7
git -C deps/fceumm apply "$PWD/patches/fceumm-q2.patch"
git clone https://github.com/libretro/snes9x2005.git deps/snes9x2005
git -C deps/snes9x2005 checkout a79dfe9047e7fec58808aefe48ad2bf499c7af11
git -C deps/snes9x2005 apply "$PWD/patches/snes9x2005-q2.patch"
FCEUMM="$PWD/deps/fceumm" SNES9X2005="$PWD/deps/snes9x2005" \
  sh src/build-home-cores.sh dist/.boy
```

For an SD release, zip the contents of `dist` at the archive root,
including the hidden `.boy` directory.

Build the itch.io helper with Go 1.23 or later and Python 3. The Go packages
are vendored in `src/itch-helper`.

```sh
sh src/build-itch.sh dist/.boy/q2-boy-itch
cat /usr/share/ca-certificates/mozilla/*.crt > dist/.boy/itch-ca.pem
```

The certificate command uses Debian's Mozilla root certificates. Include
the corresponding certificate attribution and Go license in a release source
archive. Use root certificates from a maintained source for other build systems.

## Optional firmware bridge

On firmware with the upstream `audio2` card-app capability, Pocket Boy uses
the generic audio interface. The `gameview` capability adds the Now Playing
handoff and the force-close shortcut. See the firmware's card-app protocol in
[Q2-Pod internals](https://github.com/DiamondBond/q2-pod/blob/main/docs/internals.md#card-apps).

### Older custom firmware

The frontend runs on standard Q2-Pod 1.8.8 with game audio. The
[optional bridge patch](../firmware/q2-pod-boy-audio.patch) lets the
`.boy` card app switch audio sources and open native Now Playing.
It targets commit `633f7b9e8df4d1278f39fd14d31e01cb51b9623e`.
Apply the patch at the Q2-Pod checkout root and follow its development build
instructions with the pinned Shanling Q2 V1.36 ZIP. The proposed upstream
change is tracked in [PR #426](https://github.com/DiamondBond/q2-pod/pull/426).

To build with idle dimming, apply [the display patch](../firmware/q2-pod-boy-display.patch)
after the audio patch. The display bridge dims after 30 seconds on a library or
menu screen. The firmware restores its configured brightness on wake, native
Now Playing, and child exit, including a crash.

## Smoke test

`tests/smoke.s` is a tiny homebrew GBA program and
`python3 tests/make-gb-fixtures.py /path/to/fixtures` generates original
GB and GBC test ROMs. Run `python3 tests/make-home-fixtures.py /path/to/fixtures`
for original NES and SNES test ROMs. Pass all of them to the Q2 binary to
exercise all four cores in one process.

```sh
q2-boy --smoke test.gba /path/to/existing-test-directory \
  core-qa.gb core-qa.gbc core-qa.nes core-qa.sfc core-qa.smc
```

Tests run against Q2 firmware libraries in a MIPS emulator. Use the physical
Q2 for playback, display, and input checks.

For the audio and Now Playing protocol test, build on a Linux host and run
the binary from an empty scratch directory.

```sh
cc -D_GNU_SOURCE -DQ2_RUNTIME_DIR='"."' -std=gnu11 -O2 -ffunction-sections \
  -fdata-sections -Wl,--gc-sections -I"$GPSP/libretro/libretro-common/include" \
  tests/audio-view.c src/library_paths.c -o /tmp/q2-audio-view-test
(cd "$(mktemp -d)" && /tmp/q2-audio-view-test)
```
