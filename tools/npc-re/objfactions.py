# objfactions.py : for every objective class in the MetaAI manager's list that holds units the
# way spawners do (+0xa0 {ptr, n << 17} of objects whose +0x38 archetype links to a faction), count
# units per (objective class vtable, faction id). Shows which factions live in guard posts, camps,
# civilian spawners... Walks the manager's list only.
import collections, struct
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
lst, n = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, n * 8)


def obj_of_link(link):
    if not link: return 0
    o, f = L.q(link), L.q(link + 8)
    return o if (f is not None and (f >> 31) & 1) else 0


FV = L.base + 0x3789a88  # MetaAIFactionData
cnt = collections.Counter()
for i in range(n):
    o = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(o, 0xb0) if o else None
    if not h: continue
    vt = struct.unpack_from('<Q', h, 0)[0] - L.base
    a, c = struct.unpack_from('<QI', h, 0xa0)
    c >>= 17
    if not a or not (0 < c <= 4096): continue
    us = L.read(a, 8 * c)
    if not us: continue
    for u in struct.unpack('<%dQ' % c, us):
        uh = L.read(u, 0x40) if u else None
        if not uh: continue
        ar = obj_of_link(struct.unpack_from('<Q', uh, 0x38)[0])
        if not ar: continue
        fl = L.q(ar + 0x10)
        f = obj_of_link(fl)
        if not f or L.q(f) != FV: continue
        cnt[(vt, L.q(fl + 0x10), (struct.unpack_from('<Q', uh, 0)[0] - L.base))] += 1
for (vt, fid, uvt), c in sorted(cnt.items(), key=lambda kv: -kv[1])[:40]:
    print('objective vt %x  faction %x  unit vt %x : %d units' % (vt, fid, uvt, c))
