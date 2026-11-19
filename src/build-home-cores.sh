#!/bin/sh
set -eu
: "${SYSROOT:?Set SYSROOT to the Q2 sysroot}"
: "${FCEUMM:?Set FCEUMM to the FCEUmm source checkout}"
: "${SNES9X2005:?Set SNES9X2005 to the Snes9x 2005 source checkout}"
CC_Q2=${CC_Q2:-clang}
LD_Q2=${LD_Q2:-ld.lld}
out=$(realpath -m "${1:-.}")
mkdir -p "$out"
target="--target=mipsel-linux-gnu --sysroot=$SYSROOT -isystem $SYSROOT/usr/include/mipsel-linux-gnu -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008"
flags="-Wl,--no-undefined,--hash-style=sysv,-z,noexecstack,-s"
libs="$SYSROOT/lib/libgcc_s.so.1 $SYSROOT/lib/libc.so.6 $SYSROOT/lib/libm.so.6 $SYSROOT/lib/ld-linux-mipsn8.so.1"
driver="$CC_Q2 $target -nostdlib -fuse-ld=$LD_Q2"

# Disable optional HD packs and NTSC filtering to keep video at the NES resolution.
make -C "$FCEUMM" -j "${JOBS:-4}" platform=unix HAVE_HDPACK=0 HAVE_NTSC=0 WANT_32BPP=0 \
    CC="$CC_Q2 $target" LD="$driver" LDFLAGS="$flags" LIBS="$libs" \
    TARGET="$out/fceumm_libretro.so"

# Build the standard core with its lighter audio engine.
make -C "$SNES9X2005" -j "${JOBS:-4}" platform=unix USE_BLARGG_APU=0 \
    CC="$CC_Q2 $target" LD="$driver" LDFLAGS="$flags" LIBS="$libs" \
    TARGET="$out/snes9x2005_libretro.so"
