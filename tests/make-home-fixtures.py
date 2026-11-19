"""Emit tiny original NES and SNES cartridge programs for the Q2 smoke test."""
from pathlib import Path
import struct
import sys


def nes():
    # Reset PPU, wait for two vblanks, set backdrop color and write battery RAM.
    code = bytes.fromhex(
        "78 d8 a2 ff 9a e8 8e 00 20 8e 01 20"
        "2c 02 20 10 fb 2c 02 20 10 fb"
        "a9 3f 8d 06 20 a9 00 8d 06 20 a9 16 8d 07 20"
        "a9 00 8d 06 20 8d 06 20 a9 0a 8d 01 20"
        "a9 42 8d 00 60"
    )
    loop = 0x8000 + len(code)
    code += b"\x4c" + struct.pack("<H", loop)
    prg = bytearray(16 * 1024)
    prg[:len(code)] = code
    struct.pack_into("<3H", prg, len(prg) - 6, 0x8000, 0x8000, 0x8000)
    return b"NES\x1a" + bytes([1, 1, 2, 0]) + bytes(8) + prg + bytes(8 * 1024)


def snes():
    # Set CGRAM entry zero to red, then turn the display on.
    code = bytes.fromhex(
        "78 d8 a9 80 8d 00 21 9c 21 21"
        "a9 1f 8d 22 21 9c 22 21 a9 0f 8d 00 21"
        "a9 42 8f 00 00 70 80 fe"
    )
    rom = bytearray(32 * 1024)
    rom[:len(code)] = code
    rom[0x7FC0:0x7FD5] = b"Q2 SNES CORE QA".ljust(21, b" ")
    rom[0x7FD5:0x7FDC] = bytes([0x20, 2, 5, 1, 1, 0x33, 0])
    for address in (0x7FE4, 0x7FE6, 0x7FE8, 0x7FEA, 0x7FEE, 0x7FF4, 0x7FF8, 0x7FFA, 0x7FFC, 0x7FFE):
        struct.pack_into("<H", rom, address, 0x8000)
    checksum = (sum(rom) + 510) & 0xFFFF
    struct.pack_into("<2H", rom, 0x7FDC, checksum ^ 0xFFFF, checksum)
    return rom


out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
(out / "core-qa.nes").write_bytes(nes())
(out / "core-qa.sfc").write_bytes(snes())
(out / "core-qa.smc").write_bytes(bytes(512) + snes())
