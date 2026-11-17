# mgrunits.py : MetaAI virtual units (vtable rva 0x377eab8) in the MetaAI manager's objective list
# (World+0x1688 -> +0x7b0 {ptr, n << 17}) by distance from the player, and how many are activated
# (+0x1b8 activation record set). Reads only the list (no heap scan), so it is cheap enough to run
# while the game is busy.
import math, struct, collections
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
lst, n = L.q(M + 0x7b0), L.d(M + 0x7b8) >> 17
b = L.read(lst, n * 8) or b''
bands = [20, 40, 80, 120, 250]
tot, act = collections.Counter(), collections.Counter()
UV = L.base + 0x377eab8
for i in range(len(b) // 8):
    o = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(o, 0x1c0) if o else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != UV: continue
    x, y = struct.unpack_from('<ff', h, 0x10)
    d = math.hypot(x - px, y - py)
    a = struct.unpack_from('<Q', h, 0x1b8)[0] != 0
    for r in bands:
        if d <= r:
            tot[r] += 1
            act[r] += a
print('objective list %d | ' % n + ' '.join('<=%dm: %d units (%d active) |' % (r, tot[r], act[r]) for r in bands))
