#!/usr/bin/env python3
"""
Generate a BPS patch that converts vanilla SSB64 NTSC-U into a Frame
Zero edition: the OPTION mode-select case is rewired to write
IPC_OPEN_OVERLAY=1 to the magic RDRAM address 0x807FFFE0 instead of
loading the OPTION scene. Selecting OPTION in-game thus signals the
emulator to start the Frame Zero connect flow.

7-instruction patch at ROM 0x11D6F4..0x11D70F (VRAM 0x80132764..
0x8013277F), worked out in stage 2 (commit 56b0b649). Untouched:
the audio cue at 0x80132758 + delay slot, the beq-to-epilogue at
0x80132780 + delay slot.

Output: <out>.bps applies cleanly to baserom.us.z64 (md5
f7c52568a31aadf26e14dc2b6416b2ed).

Run from repo root:
    python Source/Script/fz_make_bps.py path/to/baserom.us.z64 frame-zero.bps
"""
import hashlib
import struct
import sys
import zlib

VANILLA_MD5 = "f7c52568a31aadf26e14dc2b6416b2ed"

# (rom_offset, original_word, patched_word) — sanity-check + patch.
PATCHES = [
    (0x11D6F4, 0x3C03800A, 0x3C038080),  # lui  $3, 0x800a    -> lui  $3, 0x8080
    (0x11D6F8, 0x24634AD0, 0x2463FFE0),  # addiu $3,$3,0x4ad0 -> addiu $3,$3,-32   (=> 0x807FFFE0)
    (0x11D6FC, 0x90780000, 0x00000000),  # lbu  $24, 0($3)    -> nop
    (0x11D700, 0x24190039, 0x24190001),  # addiu $25,$0,57    -> addiu $25,$0,1   (IPC_OPEN_OVERLAY)
    (0x11D704, 0xA0790000, 0xAC790000),  # sb   $25, 0($3)    -> sw   $25, 0($3)
    (0x11D708, 0x0C00171D, 0x00000000),  # jal syTaskmanSetLoadScene -> nop
    (0x11D70C, 0xA0780001, 0x00000000),  # sb   $24, 1($3)    -> nop
]

PATCH_START  = PATCHES[0][0]
PATCH_LENGTH = len(PATCHES) * 4   # 28 bytes


def encode_varint(value: int) -> bytes:
    """BPS varint: 7 bits per byte, MSB set on the LAST byte."""
    if value < 0:
        raise ValueError("BPS varint must be non-negative")
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value == 0:
            out.append(b | 0x80)
            return bytes(out)
        out.append(b)
        value -= 1


def main():
    if len(sys.argv) != 3:
        print("usage: fz_make_bps.py <baserom.us.z64> <output.bps>",
              file=sys.stderr)
        return 1

    src_path, dst_path = sys.argv[1], sys.argv[2]

    with open(src_path, "rb") as f:
        src = bytearray(f.read())

    md5 = hashlib.md5(bytes(src)).hexdigest()
    if md5 != VANILLA_MD5:
        print(f"error: source MD5 {md5} != vanilla {VANILLA_MD5}", file=sys.stderr)
        return 2

    print(f"source: {src_path} ({len(src)} bytes, md5 ok)")

    # Verify the ROM has the bytes we expect at the patch sites, then
    # build the patched target image in memory.
    target = bytearray(src)
    for rom_off, expect_word, patched_word in PATCHES:
        actual = struct.unpack(">I", bytes(target[rom_off:rom_off + 4]))[0]
        if actual != expect_word:
            print(f"error: at {rom_off:#x} expected {expect_word:#010x} but found {actual:#010x}",
                  file=sys.stderr)
            return 3
        struct.pack_into(">I", target, rom_off, patched_word)

    print(f"patched: {len(PATCHES)} instructions at ROM "
          f"{PATCH_START:#x}..{PATCH_START + PATCH_LENGTH - 1:#x}")

    # Build the BPS body. Three actions:
    #   1. SourceRead PATCH_START bytes
    #   2. TargetRead PATCH_LENGTH bytes (literal new instructions)
    #   3. SourceRead (size - PATCH_START - PATCH_LENGTH) bytes
    # In BPS, SourceRead reads from outputPos in the source, and
    # outputPos auto-advances. So the third SourceRead will pick up
    # where the TargetRead left off.

    def action_header(length: int, kind: int) -> bytes:
        return encode_varint(((length - 1) << 2) | kind)

    body = bytearray()
    body += action_header(PATCH_START, 0)            # SourceRead PATCH_START
    body += action_header(PATCH_LENGTH, 1)           # TargetRead PATCH_LENGTH
    body += bytes(target[PATCH_START:PATCH_START + PATCH_LENGTH])
    tail_len = len(src) - PATCH_START - PATCH_LENGTH
    body += action_header(tail_len, 0)               # SourceRead remainder

    # Compute CRC32 of source / target / patch.
    src_crc = zlib.crc32(bytes(src)) & 0xFFFFFFFF
    tgt_crc = zlib.crc32(bytes(target)) & 0xFFFFFFFF

    out = bytearray()
    out += b"BPS1"
    out += encode_varint(len(src))
    out += encode_varint(len(target))
    out += encode_varint(0)                          # no metadata
    out += body
    out += struct.pack("<I", src_crc)
    out += struct.pack("<I", tgt_crc)
    patch_crc = zlib.crc32(bytes(out)) & 0xFFFFFFFF
    out += struct.pack("<I", patch_crc)

    with open(dst_path, "wb") as f:
        f.write(bytes(out))

    print(f"wrote: {dst_path} ({len(out)} bytes)")
    print(f"  src crc32:   {src_crc:#010x}")
    print(f"  target crc32:{tgt_crc:#010x}")
    print(f"  patch crc32: {patch_crc:#010x}")
    print(f"  target md5:  {hashlib.md5(bytes(target)).hexdigest()}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
