#!/usr/bin/env python3
"""
Apply the Frame Zero patch directly to a SSB64 NTSC-U ROM, producing a
patched ROM file. Equivalent to running fz_make_bps.py through flips,
but doesn't require any external tooling. Useful for local testing.

Run:
    python Source/Script/fz_apply_patch.py baserom.us.z64 frame-zero.us.z64
"""
import hashlib
import struct
import sys

# Same as fz_make_bps.py
VANILLA_MD5 = "f7c52568a31aadf26e14dc2b6416b2ed"

PATCHES = [
    (0x11D6F4, 0x3C03800A, 0x3C038080),
    (0x11D6F8, 0x24634AD0, 0x2463FFE0),
    (0x11D6FC, 0x90780000, 0x00000000),
    (0x11D700, 0x24190039, 0x24190001),
    (0x11D704, 0xA0790000, 0xAC790000),
    (0x11D708, 0x0C00171D, 0x00000000),
    (0x11D70C, 0xA0780001, 0x00000000),
]


def main():
    if len(sys.argv) != 3:
        print("usage: fz_apply_patch.py <baserom.us.z64> <output.us.z64>",
              file=sys.stderr)
        return 1

    src_path, dst_path = sys.argv[1], sys.argv[2]

    with open(src_path, "rb") as f:
        rom = bytearray(f.read())

    md5 = hashlib.md5(bytes(rom)).hexdigest()
    if md5 != VANILLA_MD5:
        print(f"error: source MD5 {md5} != vanilla {VANILLA_MD5}",
              file=sys.stderr)
        return 2

    for rom_off, expect_word, patched_word in PATCHES:
        actual = struct.unpack(">I", bytes(rom[rom_off:rom_off + 4]))[0]
        if actual != expect_word:
            print(f"error: at {rom_off:#x} expected {expect_word:#010x} "
                  f"but found {actual:#010x}", file=sys.stderr)
            return 3
        struct.pack_into(">I", rom, rom_off, patched_word)

    with open(dst_path, "wb") as f:
        f.write(bytes(rom))

    new_md5 = hashlib.md5(bytes(rom)).hexdigest()
    print(f"patched ROM: {dst_path}")
    print(f"  patches: {len(PATCHES)} instructions at ROM "
          f"0x{PATCHES[0][0]:X}..0x{PATCHES[-1][0] + 3:X}")
    print(f"  md5: {new_md5}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
