# spawnerunits.py : for each virtual spawner (vtable rva 0x377d218), follow +0x38 (link -> objective)
# and find the MetaAIObjectiveSpawnerCustomData it uses; compare unit count (+0xa8 >> 17) with the
# number of spawn positions in the custom data (+0x84 >> 17).
import collections, json, os, struct, sys
from live import Live

L = Live()
names = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vtnames.json')))
CD = L.base + 0x3777e30


def deref_link(p):
    q = L.q(p)
    if q and 0x10000 < q < 0x7fff00000000 and L.q(q) and L.base <= L.q(q) < L.base + 0x10000000: return q
    return q


def find_cd(obj, depth=2, seen=None):
    """Breadth: pointers in obj (0x200 bytes) to a custom-data object, following link records."""
    if not obj or depth < 0: return None
    seen = seen or set()
    if obj in seen: return None
    seen.add(obj)
    b = L.read(obj, 0x200)
    if not b: return None
    for o in range(0, 0x200, 8):
        p = struct.unpack_from('<Q', b, o)[0]
        if not (0x10000 < p < 0x7fff00000000): continue
        if L.q(p) == CD: return p
        q = L.q(p)
        if q and 0x10000 < q < 0x7fff00000000 and L.q(q) == CD: return q
    for o in (0x38, 0x60):
        p = struct.unpack_from('<Q', b, o)[0]
        t = deref_link(p) if p else None
        r = find_cd(t, depth - 1, seen)
        if r: return r
    return None


c = collections.Counter()
ex = {}
for a in L.vt(0x377d218)[:int(sys.argv[1]) if len(sys.argv) > 1 else 400]:
    units = L.d(a + 0xa8) >> 17
    cd = find_cd(a)
    pos = (L.d(cd + 0x84) >> 17) if cd else -1
    c[(units, pos)] += 1
    ex.setdefault((units, pos), (hex(a), hex(cd) if cd else None))
for k, v in c.most_common(20): print('units=%d positions=%d: %d  e.g. %s' % (k[0], k[1], v, ex[k]))
