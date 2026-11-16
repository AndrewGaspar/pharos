# refs.py <addr hex> [size hex] : list the qwords in a live object that point at objects of known
# classes (directly, or through a link record), with their offsets.
import json, os, sys
from live import Live

L = Live()
names = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vtnames.json')))
a = int(sys.argv[1], 16)
size = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x400


def cls(p):
    v = L.q(p) if p else None
    if v and L.base <= v < L.base + 0x10000000:
        return names.get('%x' % (v - L.base), 'vt%x' % (v - L.base))
    return None


for o in range(0, size, 8):
    p = L.q(a + o)
    if not p or not (0x10000 < p < 0x7fff00000000): continue
    c = cls(p)
    if c:
        print('+%x -> %x %s' % (o, p, c))
        continue
    q = L.q(p)
    if q and 0x10000 < q < 0x7fff00000000:
        c = cls(q)
        if c: print('+%x -> %x -> %x %s' % (o, p, q, c))
