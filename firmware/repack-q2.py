"""Substitute one verified demo binary into the corresponding 1.8.8 release."""
import hashlib
import io
import pathlib
import re
import shlex
import struct
import subprocess
import sys
import tarfile

source, demo, destination, build_dir = map(pathlib.Path, sys.argv[1:])
build_dir.mkdir(parents=True, exist_ok=True)
source_sq = build_dir / "base.squashfs"
new_sq = build_dir / "rootfs.squashfs"
pseudo = build_dir / "root.pseudo"
empty = build_dir / "empty"
empty.mkdir(exist_ok=True)
with tarfile.open(source) as base:
    entries = base.getmembers()
    blobs = {e.name: base.extractfile(e).read() for e in entries if e.isfile()}
info_name = "firmware_v20.info"
root_name = "recovery-update/rootfs.squashfs"
kernel_name = "recovery-update/xImage"
assert blobs[info_name].splitlines()[:2] == [b"Shanling Q2", b"V1880"]
assert b"Q2_GBA_AUDIO_BRIDGE" in demo.read_bytes()
source_sq.write_bytes(blobs[root_name])
subprocess.run(["unsquashfs", "-pf", str(pseudo), str(source_sq)], check=True, stdout=subprocess.DEVNULL)
data = pseudo.read_bytes()
root = re.search(rb"^/ D (\d+) (\d+) (\d+) (\d+)$", data, re.M)
old = re.search(rb"^release/bin/demo R (\d+) (\d+) (\d+) (\d+) .+$", data, re.M)
assert root and old
replacement = b"release/bin/demo F " + b" ".join(old.groups()) + b" cat " + shlex.quote(str(demo.resolve())).encode()
pseudo.write_bytes(data[:old.start()] + replacement + data[old.end():])
stamp, mode, uid, gid = [x.decode() for x in root.groups()]
epoch = struct.unpack_from("<I", blobs[root_name], 8)[0]
subprocess.run([
    "mksquashfs", str(empty), str(new_sq), "-pf", str(pseudo), "-noappend", "-comp", "lzo",
    "-b", "131072", "-Xcompression-level", "9", "-tailends", "-mkfs-time", str(epoch),
    "-root-time", stamp, "-root-mode", mode, "-root-uid", uid, "-root-gid", gid,
    "-processors", "1", "-no-progress"
], check=True, stdout=subprocess.DEVNULL)

def inodes(image):
    raw = subprocess.check_output(["unsquashfs", "-pf", "-", str(image)])
    lines = raw.split(b"\n# START OF DATA")[0].splitlines()
    return sorted(line.split()[:6] for line in lines if line and not line.startswith(b"#"))

assert inodes(source_sq) == inodes(new_sq)
assert subprocess.check_output(["unsquashfs", "-cat", str(new_sq), "release/bin/demo"]) == demo.read_bytes()
blobs[root_name] = new_sq.read_bytes()
assert len(blobs[root_name]) <= 50688000
blobs[info_name] = ("Shanling Q2\nV1887\n" + "".join(
    f"{hashlib.md5(blobs[name]).hexdigest()}  {name}\n" for name in (kernel_name, root_name)
)).encode()
with tarfile.open(destination, "w", format=tarfile.GNU_FORMAT) as output:
    for entry in entries:
        if entry.isfile():
            blob = blobs[entry.name]
            entry.size = len(blob)
            output.addfile(entry, io.BytesIO(blob))
        else:
            output.addfile(entry)
print("Verified exact kernel, unchanged inode metadata, patched demo and image size.")
