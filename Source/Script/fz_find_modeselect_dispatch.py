#!/usr/bin/env python3
"""
Find the mnModeSelectFuncRun dispatch tail in vanilla SSB64 NTSC-U.

Strategy: the dispatch loads four unique immediates (the SCKind enum
values for 1PMode/VSMode/Option/Data) close together via
`addiu rN, $zero, imm`. We scan the OVL17 .text region for clusters
of these patterns and report candidate offsets.

Run from repo root:
    python Source/Script/fz_find_modeselect_dispatch.py path/to/baserom.us.z64
"""
import sys
import struct

# OVL17 (mnmodeselect) bounds in baserom.us.z64, from
# ssb-decomp-re/smashbrothers.us.yaml:
OVL17_ROM_START = 0x11CA90
OVL17_TEXT_END  = 0x11DB20  # .data section start
OVL17_VRAM      = 0x80131B00

# NTSC-U SCKind enum values used by the four dispatch cases:
SCK_1PMODE = 0x08
SCK_VSMODE = 0x09
SCK_OPTION = 0x39
SCK_DATA   = 0x3A

# Cluster window — within how many bytes of each other should all
# four immediates appear?
WINDOW_BYTES = 200


def find_addiu_immediates(text: bytes, base_offset: int, want_imm: int):
    """Find every `addiu rN, $zero, imm` instruction with the given imm.
    Returns a list of (rom_offset, register_id) tuples."""
    hits = []
    # MIPS instruction is big-endian 32-bit. addiu rN, $zero, imm:
    #   [31:26]=001001 [25:21]=00000 [20:16]=rt [15:0]=imm
    # Top two bytes: 0x24 0x0N  (where N = rt's low nibble; bit 4 of rt
    # is the low bit of byte 1, so 0x00..0x1F).
    for off in range(0, len(text) - 4 + 1, 4):
        word = struct.unpack(">I", text[off:off + 4])[0]
        opcode = (word >> 26) & 0x3F
        rs     = (word >> 21) & 0x1F
        rt     = (word >> 16) & 0x1F
        imm    = word & 0xFFFF
        if opcode == 0b001001 and rs == 0 and imm == want_imm:
            hits.append((base_offset + off, rt))
    return hits


def main():
    if len(sys.argv) != 2:
        print("usage: fz_find_modeselect_dispatch.py <baserom.us.z64>",
              file=sys.stderr)
        return 1

    with open(sys.argv[1], "rb") as f:
        rom = f.read()

    text = rom[OVL17_ROM_START:OVL17_TEXT_END]
    print(f"OVL17 .text: {OVL17_ROM_START:#x}..{OVL17_TEXT_END:#x} "
          f"({len(text)} bytes), VRAM base {OVL17_VRAM:#x}")

    hits_1p     = find_addiu_immediates(text, OVL17_ROM_START, SCK_1PMODE)
    hits_vs     = find_addiu_immediates(text, OVL17_ROM_START, SCK_VSMODE)
    hits_option = find_addiu_immediates(text, OVL17_ROM_START, SCK_OPTION)
    hits_data   = find_addiu_immediates(text, OVL17_ROM_START, SCK_DATA)

    # SCK_1PMODE = 8 is extremely common as a generic constant. We
    # emphasise OPTION and DATA which are uniquely menu-related, then
    # corroborate with neighbouring 1PMODE / VSMODE.
    print("\n=== addiu rN, $zero, 0x39 (nSCKindOption) ===")
    for off, rt in hits_option:
        print(f"  rom={off:#x}  vram={off - OVL17_ROM_START + OVL17_VRAM:#x}  rt=${rt}")

    print("\n=== addiu rN, $zero, 0x3A (nSCKindData) ===")
    for off, rt in hits_data:
        print(f"  rom={off:#x}  vram={off - OVL17_ROM_START + OVL17_VRAM:#x}  rt=${rt}")

    print("\n=== addiu rN, $zero, 0x09 (nSCKindVSMode) ===")
    for off, rt in hits_vs:
        print(f"  rom={off:#x}  vram={off - OVL17_ROM_START + OVL17_VRAM:#x}  rt=${rt}")

    print("\n=== addiu rN, $zero, 0x08 (nSCKind1PMode) ===")
    for off, rt in hits_1p:
        print(f"  rom={off:#x}  vram={off - OVL17_ROM_START + OVL17_VRAM:#x}  rt=${rt}")

    # Lightweight disassembly helper. Decodes the few opcodes we
    # actually want to see when staring at a candidate dispatch:
    # addiu / addi / lui / ori / sw / lw / jal / j / jr / nop / sll.
    def decode(word: int) -> str:
        op = (word >> 26) & 0x3F
        rs = (word >> 21) & 0x1F
        rt = (word >> 16) & 0x1F
        rd = (word >> 11) & 0x1F
        sa = (word >> 6)  & 0x1F
        fn = word & 0x3F
        imm  = word & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        tgt  = (word & 0x03FFFFFF) << 2

        if word == 0:
            return "nop"
        if op == 0:
            if fn == 0x00 and word != 0:
                return f"sll ${rd}, ${rt}, {sa}"
            if fn == 0x08:
                return f"jr ${rs}"
            if fn == 0x09:
                return f"jalr ${rd}, ${rs}"
            if fn == 0x21:
                return f"addu ${rd}, ${rs}, ${rt}"
            if fn == 0x23:
                return f"subu ${rd}, ${rs}, ${rt}"
            if fn == 0x25:
                return f"or ${rd}, ${rs}, ${rt}"
            return f"special fn={fn:#x}"
        if op == 0x02:
            return f"j {tgt:#x}"
        if op == 0x03:
            return f"jal {tgt:#x}  (vram base depends on overlay)"
        if op == 0x04:
            return f"beq ${rs}, ${rt}, {simm * 4 + 4:#x}"
        if op == 0x05:
            return f"bne ${rs}, ${rt}, {simm * 4 + 4:#x}"
        if op == 0x09:
            return f"addiu ${rt}, ${rs}, {simm}"
        if op == 0x0D:
            return f"ori ${rt}, ${rs}, {imm:#x}"
        if op == 0x0F:
            return f"lui ${rt}, {imm:#x}"
        if op == 0x23:
            return f"lw ${rt}, {simm}(${rs})"
        if op == 0x2B:
            return f"sw ${rt}, {simm}(${rs})"
        return f"op={op:#x} rs={rs} rt={rt} imm={imm:#x}"

    def dump_window(label: str, center_off: int, before: int = 8, after: int = 8):
        """Disassemble a small window around a ROM offset."""
        start = center_off - before * 4
        end   = center_off + after  * 4 + 4
        if start < OVL17_ROM_START:
            start = OVL17_ROM_START
        if end > OVL17_TEXT_END:
            end = OVL17_TEXT_END
        print(f"\n--- {label} (center rom={center_off:#x}) ---")
        for off in range(start, end, 4):
            word = struct.unpack(">I", rom[off:off+4])[0]
            mark = " <-- " if off == center_off else "     "
            print(f"  rom={off:#x}  vram={off - OVL17_ROM_START + OVL17_VRAM:#x}"
                  f"  {word:08x}  {mark}{decode(word)}")

    # Cluster: report each (option, data, vs, 1p) tuple within WINDOW_BYTES
    print(f"\n=== clusters (all four within {WINDOW_BYTES} bytes) ===")
    found_cluster = False
    for off_o, _ in hits_option:
        for off_d, _ in hits_data:
            if abs(off_o - off_d) > WINDOW_BYTES:
                continue
            near_vs = [o for o, _ in hits_vs   if abs(o - off_o) <= WINDOW_BYTES]
            near_1p = [o for o, _ in hits_1p   if abs(o - off_o) <= WINDOW_BYTES]
            if near_vs and near_1p:
                lo = min(off_o, off_d, *near_vs, *near_1p)
                hi = max(off_o, off_d, *near_vs, *near_1p)
                print(f"  rom {lo:#x}..{hi:#x}  span={hi-lo}  "
                      f"option@{off_o:#x} data@{off_d:#x} "
                      f"vs={[hex(x) for x in near_vs]} "
                      f"1p={[hex(x) for x in near_1p]}")
                found_cluster = True
    if not found_cluster:
        print("  none — patterns may be encoded differently "
              "(e.g. ori with lui, or sw of a pre-loaded constant).")

    # Detailed dump around each OPTION hit so we can pick the dispatch
    # one by sight (look for `jal` to syTaskmanSetLoadScene + `sw` to
    # gSCManagerSceneData fields).
    for off, _ in hits_option:
        dump_window(f"around OPTION immediate at {off:#x}", off,
                    before=10, after=12)

    return 0


if __name__ == "__main__":
    sys.exit(main())
