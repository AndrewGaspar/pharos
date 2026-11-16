# hist.py <out.json> : histogram of live heap objects per class (first qword == a known vtable).
# hist.py --diff a.json b.json [min] : classes whose counts changed most between two snapshots.
import json, os, sys
import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
names = json.load(open(os.path.join(here, 'vtnames.json')))

if sys.argv[1] == '--diff':
    a, b = json.load(open(sys.argv[2])), json.load(open(sys.argv[3]))
    mn = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    rows = []
    for k in set(a) | set(b):
        x, y = a.get(k, 0), b.get(k, 0)
        if abs(y - x) >= mn: rows.append((y - x, x, y, k))
    for dlt, x, y, k in sorted(rows, key=lambda r: -abs(r[0]))[:80]:
        print('%6d -> %6d (%+d)  %s' % (x, y, dlt, k))
    sys.exit()

from live import Live
L = Live()
vts = np.array(sorted(L.base + int(k, 16) for k in names), dtype=np.uint64)
counts = {}
for s, e in L.heap_regions():
    for cs in range(s, e, 64 << 20):
        ce = min(e, cs + (64 << 20))
        buf = L.read(cs, ce - cs)
        if not buf: continue
        a = np.frombuffer(buf[:len(buf) // 16 * 16], dtype='<u8')[::2]  # objects are 16-byte aligned
        hit = a[np.isin(a, vts)]
        u, c = np.unique(hit, return_counts=True)
        for v, n in zip(u, c):
            key = names['%x' % (int(v) - L.base)]
            counts[key] = counts.get(key, 0) + int(n)
json.dump(counts, open(sys.argv[1], 'w'), indent=0, sort_keys=True)
print(len(counts), 'classes,', sum(counts.values()), 'objects')
