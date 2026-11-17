# urbanity.py : the urban/rural measure npc_units uses, around the player and as a distribution over
# the civilian-spawner units within 400 m: civilian-spawner human units (vtable 0x377eab8) per 50 m
# cell, summed over the 7 x 7 cells around a point (~350 m square). Run it with npc_units=1 (or on a
# game whose extras are not counted) to calibrate [world] npc_urban_threshold.
import collections, math, struct
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
lst, n = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, n * 8)
SV, UV = L.base + 0x377d218, L.base + 0x377eab8
grid = collections.Counter()
pts = []
for i in range(n):
    s = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(s, 0xd0) if s else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != SV: continue
    a, c = struct.unpack_from('<QI', h, 0xa0)
    c >>= 17
    if not a or c > 16384: continue
    for u in struct.unpack('<%dQ' % c, L.read(a, 8 * c) or b'\0' * 8 * c):
        uh = L.read(u, 0x20) if u else None
        if not uh or struct.unpack_from('<Q', uh, 0)[0] != UV: continue
        x, y = struct.unpack_from('<ff', uh, 0x10)
        grid[(math.floor(x / 50), math.floor(y / 50))] += 1
        if math.hypot(x - px, y - py) < 400: pts.append((x, y))


def near(x, y):
    cx, cy = math.floor(x / 50), math.floor(y / 50)
    return sum(grid[(cx + dx, cy + dy)] for dx in range(-3, 4) for dy in range(-3, 4))


vals = sorted(near(x, y) for x, y in pts)
q = lambda f: vals[min(len(vals) - 1, int(f * len(vals)))] if vals else 0
print('player at %.0f %.0f: %d units near the player; units within 400 m: %d, their measure p10 %d / median %d / p90 %d' % (
    px, py, near(px, py), len(vals), q(0.1), q(0.5), q(0.9)))
