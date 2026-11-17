# crowdsys.py : look for the crowd systems that could carry a cheap far-field population, reachable
# from the World object without a heap scan: World fields (0x0-0x2000) that point at objects whose
# vtable belongs to a class named Bulk*/FakeCrowd*/Crowd* (TypeInfo-derived names from vtnames.json,
# plus the TypeInfo ctors' vtables for FakeCrowdHerder / BulkWorldComponent), and one level below.
import json, os, struct
from live import Live

L = Live()
here = os.path.dirname(os.path.abspath(__file__))
names = json.load(open(os.path.join(here, 'vtnames.json')))
extra = {0x384f1a0: 'FakeCrowdHerder', 0x36406c8: 'BulkWorldComponent'}
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)


def cls(p):
    v = L.q(p) if p else None
    if not v or not (L.base <= v < L.base + 0x5000000): return None
    r = v - L.base
    return extra.get(r) or names.get('%x' % r) or 'vt%x' % r


seen = set()
b = L.read(W, 0x2000)
for o in range(0, 0x2000, 8):
    p = struct.unpack_from('<Q', b, o)[0]
    if not (0x10000 < p < 0x7fff00000000) or p in seen: continue
    seen.add(p)
    c = cls(p)
    if not c: continue
    if any(k in c for k in ('Bulk', 'FakeCrowd', 'Crowd')):
        print('World+%x -> %x %s' % (o, p, c))
    sub = L.read(p, 0x400) or b''
    for so in range(0, len(sub) - 7, 8):
        q = struct.unpack_from('<Q', sub, so)[0]
        if not (0x10000 < q < 0x7fff00000000) or q in seen: continue
        c2 = cls(q)
        if c2 and any(k in c2 for k in ('Bulk', 'FakeCrowd', 'Crowd')):
            seen.add(q)
            print('World+%x (%s) +%x -> %x %s' % (o, c, so, q, c2))
