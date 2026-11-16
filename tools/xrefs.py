#!/usr/bin/env python3
"""Find code/data references to RVAs in an in-memory image dump (file offset == RVA).

usage: xrefs.py <image> <load-base-hex> <rva-hex|rva-dec> [...]

Reports RIP-relative references (any instruction whose disp32 resolves to the target, assuming
the disp32 ends the instruction or is followed by an imm8/imm32) in executable sections, and
absolute 8-byte pointers (load-base + rva) anywhere in the image.
"""
import struct
import sys

import numpy as np

path, base = sys.argv[1], int(sys.argv[2], 16)
targets = [int(a, 0) for a in sys.argv[3:]]
data = np.fromfile(path, dtype=np.uint8)
buf = data.tobytes()

pe = struct.unpack_from("<I", buf, 0x3C)[0]
nsec = struct.unpack_from("<H", buf, pe + 6)[0]
optsz = struct.unpack_from("<H", buf, pe + 20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + 40 * i
    name = buf[o:o + 8].rstrip(b"\0").decode(errors="replace")
    vsize, va = struct.unpack_from("<II", buf, o + 8)
    ch = struct.unpack_from("<I", buf, o + 36)[0]
    secs.append((name, va, vsize, ch))
exec_secs = [s for s in secs if s[3] & 0x20000000]
print("sections:", ", ".join(f"{n}@{va:#x}+{vs:#x}{'X' if ch & 0x20000000 else ''}" for n, va, vs, ch in secs))


def section_of(rva):
    for n, va, vs, ch in secs:
        if va <= rva < va + vs:
            return n
    return "?"


for t in targets:
    print(f"== target {t:#x} ({section_of(t)})")
    # RIP-relative: disp32 at position p, instruction end e in {p+4, p+5, p+8}; target = e + disp
    chunk = 1 << 24
    for n, va, vs, ch in exec_secs:
        for c0 in range(va, va + vs, chunk):
            seg = data[c0:min(va + vs, c0 + chunk) + 3]
            if len(seg) < 8:
                continue
            # little-endian int32 at every offset
            disp = seg[0:-3].astype(np.int64) | (seg[1:-2].astype(np.int64) << 8) | \
                (seg[2:-1].astype(np.int64) << 16) | (seg[3:].astype(np.int64) << 24)
            disp = np.where(disp >= 2**31, disp - 2**32, disp)
            pos = np.arange(len(disp), dtype=np.int64) + c0
            for extra in (4, 5, 8):
                hits = np.nonzero(pos + extra + disp == t)[0]
                for h in hits[:50]:
                    p = int(pos[h])
                    print(f"  rip-rel @ {p:#x} (insn end {p + extra:#x}, {n}) bytes {buf[p - 4:p + 4].hex()}")
    # absolute pointers
    needle = struct.pack("<Q", base + t)
    i = buf.find(needle)
    k = 0
    while i != -1 and k < 30:
        print(f"  abs ptr @ {i:#x} ({section_of(i)})")
        i = buf.find(needle, i + 1)
        k += 1
