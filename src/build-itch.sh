#!/bin/sh
set -eu

out=${1:?Pass the destination for q2-boy-itch}
case $out in /*) ;; *) out=$PWD/$out ;; esac
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$src/itch-helper"
GOOS=linux GOARCH=mipsle GOMIPS=softfloat CGO_ENABLED=0 "${GO_Q2:-go}" \
    build -mod=vendor -trimpath -ldflags='-s -w' -o "$out" .
# Go emits a soft-float ELF with the legacy NaN marker. The Q2 runs MIPS
# NaN2008 user space. This binary has no hardware floating-point ABI.
python3 - "$out" <<'PY'
import pathlib
import struct
import sys

file = pathlib.Path(sys.argv[1])
with file.open('r+b') as stream:
    header = stream.read(52)
    assert header[:6] == b'\x7fELF\x01\x01' and struct.unpack_from('<H', header, 18)[0] == 8
    flags = struct.unpack_from('<I', header, 36)[0]
    stream.seek(36)
    stream.write(struct.pack('<I', flags | 0x400))
PY
