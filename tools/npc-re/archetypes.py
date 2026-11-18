# archetypes.py [radius=400] : the archetypes (unit +0x38 -> MetaAIObjectiveHuman data objects) of
# civilian-spawner units within radius of the player: per archetype the unit count, its spawners,
# and the archetype object's qwords that differ between archetypes (link targets shown as the
# target object's vtable rva), to find a role / faction field. No heap scan.
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


arch = collections.Counter()
sp_of = collections.defaultdict(set)
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
        o = obj_of_link(struct.unpack_from('<Q', uh, 0x38)[0])
        if o:
            arch[o] += 1
            sp_of[o].add(s)
objs = [o for o, _ in arch.most_common()]
dumps = {o: L.read(o, 0x2d0) for o in objs}
print('%d archetypes within %g m' % (len(objs), R))
cols = []
for off in range(8, 0x2d0, 8):
    vals = set(struct.unpack_from('<Q', dumps[o], off)[0] for o in objs if dumps[o])
    if 1 < len(vals) <= 12: cols.append(off)


def show(v):
    if L.base <= v < L.base + 0x5000000: return 'rva%x' % (v - L.base)
    if 0x10000 < v < 0x7fff00000000:
        t = obj_of_link(v) or v
        vt = L.q(t)
        if vt and L.base <= vt < L.base + 0x5000000: return 'o:%x' % (vt - L.base)
        return 'p'
    return '%x' % v


print('offsets with 2-12 distinct values:', ' '.join('+%x' % c for c in cols))
for o in objs[:40]:
    print('%x units %4d spawners %3d | %s' % (o, arch[o], len(sp_of[o]), ' '.join('+%x=%s' % (c, show(struct.unpack_from('<Q', dumps[o], c)[0])) for c in cols[:10])))
