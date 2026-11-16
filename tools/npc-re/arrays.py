# arrays.py <vt rva> <size> [n] : for n instances of a class, list Anvil arrays ({ptr, u32 cap|count<<17})
# found in the object, with the class names of the pointed-to elements (element = pointer or link record).
import json, os, struct, sys
from live import Live

L = Live()
names = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vtnames.json')))
vt, size = int(sys.argv[1], 16), int(sys.argv[2], 16)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 5


def cls(p):
    v = L.q(p) if p else None
    if v and L.base <= v < L.base + 0x10000000:
        return names.get('%x' % (v - L.base), 'vt%x' % (v - L.base))
    return None


def elem(p):
    """An element pointer, or a link record {ptr, flags, id} -> pointee class."""
    c = cls(p)
    if c: return c
    q = L.q(p) if p else None
    if q and 0x10000 < q < 0x7fff00000000:
        c = cls(q)
        if c: return '*' + c
    return '?'


shown = 0
for a in L.vt(vt):
    out = []
    for o in range(8, size - 8, 4):
        p, w = L.q(a + o), L.d(a + o + 8)
        if not p or not (0x10000 < p < 0x7fff00000000) or w is None: continue
        cnt, cap = w >> 17, w & 0x7fff
        if not (1 <= cnt <= cap <= 4096): continue
        els = [elem(L.q(p + 8 * i)) for i in range(min(cnt, 6))]
        out.append('+%x[%d]: %s' % (o, cnt, ', '.join(els)))
    if out:
        print(hex(a), ' | '.join(out))
        shown += 1
    if shown >= n: break
