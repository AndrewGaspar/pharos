# mgrunits.py : MetaAI virtual units of civilian spawners, found through the MetaAI manager's list of
# virtual objectives (World+0x1688 -> +0x3b8 {ptr}, u32 count +0x3c0; spawners = vtable rva
# 0x377d218, units in their +0xa0 array), by distance from the player, with how many are activated
# (+0x1b8 activation record set). Reads only those objects (no heap scan), so it is cheap enough to
# run while the game is busy.
import math, struct, collections
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
lst, n = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, n * 8) or b''
SV = L.base + 0x377d218
bands = [20, 40, 80, 120, 250]
tot, act, dead = collections.Counter(), collections.Counter(), collections.Counter()
nsp = 0
for i in range(len(b) // 8):
    o = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(o, 0xd0) if o else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != SV: continue
    nsp += 1
    sx, sy = struct.unpack_from('<ff', h, 0x10)
    if math.hypot(sx - px, sy - py) > 400: continue
    arr, cnt = struct.unpack_from('<QI', h, 0xa0)
    cnt >>= 17
    if not arr or cnt > 4096: continue
    for u in struct.unpack('<%dQ' % cnt, L.read(arr, 8 * cnt) or b'\0' * 8 * cnt):
        uh = L.read(u, 0x1c0) if u else None
        if not uh: continue
        x, y = struct.unpack_from('<ff', uh, 0x10)
        d = math.hypot(x - px, y - py)
        a = struct.unpack_from('<Q', uh, 0x1b8)[0] != 0
        dd = uh[0x5a] & 2 != 0
        for r in bands:
            if d <= r:
                tot[r] += 1
                act[r] += a
                dead[r] += dd
print('objectives %d, spawners %d | ' % (n, nsp) + ' '.join('<=%dm: %d units (%d active, %d dead) |' % (r, tot[r], act[r], dead[r]) for r in bands))
