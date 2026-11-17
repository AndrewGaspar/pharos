# samearch.py [max m=60] : two activated civilian units with the same archetype standing close together
# (< 4 m) near the player, for a side-by-side look: prints their positions and the heading from the
# player. Walks the MetaAI manager's lists only.
import math, struct, sys
from live import Live

L = Live()
MAXD = float(sys.argv[1]) if len(sys.argv) > 1 else 60
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
lst, n = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, n * 8)
SV, UV = L.base + 0x377d218, L.base + 0x377eab8
pts = []
for i in range(n):
    s = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(s, 0xd0) if s else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != SV: continue
    a, c = struct.unpack_from('<QI', h, 0xa0)
    c >>= 17
    if not a or c > 16384: continue
    for u in struct.unpack('<%dQ' % c, L.read(a, 8 * c) or b'\0' * 8 * c):
        uh = L.read(u, 0x1c0) if u else None
        if not uh or struct.unpack_from('<Q', uh, 0)[0] != UV or not struct.unpack_from('<Q', uh, 0x1b8)[0]: continue
        x, y = struct.unpack_from('<ff', uh, 0x10)
        d = math.hypot(x - px, y - py)
        if 6 < d < MAXD: pts.append((d, u, x, y, struct.unpack_from('<Q', uh, 0x38)[0]))
pts.sort()
best = None
for i, (d, u, x, y, a) in enumerate(pts):
    for d2, u2, x2, y2, a2 in pts[i + 1:]:
        if a2 == a and math.hypot(x - x2, y - y2) < 4:
            best = (d, u, x, y, u2, x2, y2, a)
            break
    if best: break
if not best: print('none'); sys.exit(1)
d, u, x, y, u2, x2, y2, a = best
mx, my = (x + x2) / 2, (y + y2) / 2
print('%.1f %.0f units %x %x archetype link %x at (%.1f %.1f) and (%.1f %.1f)' % (
    math.hypot(mx - px, my - py), math.degrees(math.atan2(my - py, mx - px)), u, u2, a, x, y, x2, y2))
