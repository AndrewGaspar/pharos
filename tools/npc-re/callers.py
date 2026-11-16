# callers.py <rva>... : direct call/jmp rel32 sites (E8/E9) targeting each RVA, plus rip-relative refs
# (lea/mov disp32) — any 4-byte field whose value + its end address == target.
import sys
import numpy as np
from img import m

a = np.frombuffer(m, dtype=np.uint8)
views = [np.frombuffer(m, dtype='<i4', count=(len(m) - k) // 4, offset=k) for k in range(4)]
for t in [int(x, 16) for x in sys.argv[1:]]:
    calls, refs = [], []
    for k in range(4):
        v = views[k]
        pos = np.arange(len(v), dtype=np.int64) * 4 + k
        idx = np.nonzero(v.astype(np.int64) == (t - (pos + 4)))[0]
        for i in idx:
            p = int(pos[i])
            op = a[p - 1]
            (calls if op in (0xe8, 0xe9) else refs).append(p - 1)
    print('%x: calls %s' % (t, ' '.join('%x' % c for c in sorted(calls)[:40])))
    print('%x: refs  %s' % (t, ' '.join('%x' % c for c in sorted(refs)[:40])))
