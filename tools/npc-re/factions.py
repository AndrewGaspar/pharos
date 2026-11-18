# factions.py [radius=400] : factions of the civilian-spawner units near the player: archetype
# (unit +0x38 -> MetaAIObjectiveHuman) +0x10 -> link -> MetaAIFactionData. Per faction: unit count,
# archetypes, how many of its units are activated, and a dump of the faction object.
import collections, math, struct, sys
from live import Live

L = Live()
R = float(sys.argv[1]) if len(sys.argv) > 1 else 400
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
lst, n = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, n * 8)
SV = L.base + 0x377d218


def obj_of_link(link):
    if not link: return 0
    o, f = L.q(link), L.q(link + 8)
    return o if (f is not None and (f >> 31) & 1) else 0


fac_units = collections.Counter()
fac_arch = collections.defaultdict(set)
fac_link = {}
for i in range(n):
    s = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(s, 0xd0) if s else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != SV: continue
    a, c = struct.unpack_from('<QI', h, 0xa0)
    c >>= 17
    if not a or c > 16384: continue
    for u in struct.unpack('<%dQ' % c, L.read(a, 8 * c) or b'\0' * 8 * c):
        uh = L.read(u, 0x40) if u else None
        if not uh: continue
        x, y = struct.unpack_from('<ff', uh, 0x10)
        if math.hypot(x - px, y - py) > R: continue
        ar = obj_of_link(struct.unpack_from('<Q', uh, 0x38)[0])
        if not ar: continue
        fl = L.q(ar + 0x10)
        f = obj_of_link(fl)
        fac_units[f] += 1
        fac_arch[f].add(ar)
        fac_link[f] = fl
for f, cnt in fac_units.most_common():
    fid = L.q(fac_link[f] + 0x10) if fac_link[f] else 0
    d = L.read(f, 0x80) if f else None
    print('faction %x (id %x): %d units, %d archetypes' % (f, fid or 0, cnt, len(fac_arch[f])))
    if d:
        print('   ' + ' '.join('%016x' % struct.unpack_from('<Q', d, o)[0] for o in range(0, 0x80, 8)))
