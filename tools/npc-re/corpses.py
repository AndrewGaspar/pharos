# corpses.py : units restored from the save's dead-unit records (unit +0x90 bit 15, set by the load's
# 0x1dab8a0) and all dead units (+0x5a bit 1), found through the MetaAI manager's objective list
# (spawners, vtable rva 0x377d218, units at +0xa0). No heap scan.
import math, struct
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
lst, n = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, n * 8) or b''
SV = L.base + 0x377d218
for i in range(len(b) // 8):
    sp = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(sp, 0xd0) if sp else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != SV: continue
    arr, cnt = struct.unpack_from('<QI', h, 0xa0)
    cnt >>= 17
    if not arr or cnt > 4096: continue
    for u in struct.unpack('<%dQ' % cnt, L.read(arr, 8 * cnt) or b'\0' * 8 * cnt):
        uh = L.read(u, 0x98) if u else None
        if not uh: continue
        restored = struct.unpack_from('<Q', uh, 0x90)[0] & 0x8000 != 0
        dead = uh[0x5a] & 2 != 0
        if restored or dead:
            x, y, z = struct.unpack_from('<fff', uh, 0x10)
            print('%s%s unit %x spawner %x id %x at (%.1f, %.1f, %.1f), %.0f m from the player' % (
                'restored ' if restored else '', 'dead' if dead else 'alive', u, sp,
                struct.unpack_from('<I', uh, 8)[0], x, y, z, math.hypot(x - px, y - py)))
