# spawnbands.py : spawned MetaAI characters per distance band from the player, plus civilian units
# per band. The MetaAI manager (World+0x1688) keeps the spawn records of its spawned (or spawning)
# objectives in the array at +0x48 ({ptr, n << 17}, records with vtable rva 0x363be88; a unit's
# record is its +0x1b8). Its size follows the game's own spawned count (341 vs 342-355; after a
# live cap cut 207 vs 223), and records with the u32 at +0x34 == 1 are the pending ones.
# Spawned = records with +0x34 == 0; position = the unit's +0x10 (follows the character).
# Records whose unit is not in a civilian spawner (animals, guards, ...) count as "other".
# Walks the manager's lists only (no heap scan).
import math, struct, collections
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
edges = [40, 80, 120, 200, 1e9]
names = ['0-40', '40-80', '80-120', '120-200', '200+']


def band(x, y):
    d = math.hypot(x - px, y - py)
    for e, nm in zip(edges, names):
        if d <= e: return nm


rec_pos = {}
units = collections.Counter()
lst, nn = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, nn * 8) or b''
SV = L.base + 0x377d218
for i in range(len(b) // 8):
    s = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(s, 0xd0) if s else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != SV: continue
    a, c = struct.unpack_from('<QI', h, 0xa0)
    c >>= 17
    if not a or c > 4096: continue
    for u in struct.unpack('<%dQ' % c, L.read(a, 8 * c) or b'\0' * 8 * c):
        uh = L.read(u, 0x1c0) if u else None
        if not uh: continue
        x, y = struct.unpack_from('<ff', uh, 0x10)
        units[band(x, y)] += 1
        r = struct.unpack_from('<Q', uh, 0x1b8)[0]
        if r: rec_pos[r] = (x, y)
sp = collections.Counter()
other = pending = 0
arr, n = L.q(M + 0x48), L.d(M + 0x50) >> 17
for i in range(n):
    r = L.q(arr + 8 * i)
    if not r: continue
    if L.d(r + 0x34) == 1:
        pending += 1
        continue
    if r in rec_pos: sp[band(*rec_pos[r])] += 1
    else: other += 1
print('spawned civilians ' + ' '.join('%s:%d' % (nm, sp[nm]) for nm in names) +
      ' | total %d, other MetaAI %d, pending %d' % (sum(sp.values()), other, pending))
print('civilian units    ' + ' '.join('%s:%d' % (nm, units[nm]) for nm in names))
