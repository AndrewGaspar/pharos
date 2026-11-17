# rel32.py <rva>... : like callers.py, but one pass over the image for all targets (fast for many
# targets). Prints, per target, the sites whose rel32 (disp32 at the end of the instruction, assumed to
# end right after the 4 bytes: call/jmp rel32 and FF 15/FF 25 call/jmp [rip+x]) resolves to it.
# Kind: call (E8), jmp (E9), icall (FF 15: call [rip+disp]), ijmp (FF 25), ref (anything else).
import sys
import numpy as np
from img import m

targets = np.array(sorted(int(x, 16) for x in sys.argv[1:]), dtype=np.int64)
a = np.frombuffer(m, dtype=np.uint8)
hits = {int(t): [] for t in targets}
for k in range(4):
    v = np.frombuffer(m, dtype='<i4', count=(len(m) - k) // 4, offset=k)
    pos = np.arange(len(v), dtype=np.int64) * 4 + k
    tgt = pos + 4 + v.astype(np.int64)
    idx = np.nonzero(np.isin(tgt, targets))[0]
    for i in idx:
        p = int(pos[i])
        hits[int(tgt[i])].append(p)
for t, ps in hits.items():
    out = []
    for p in sorted(ps):
        op = a[p - 1]
        kind = 'call' if op == 0xe8 else 'jmp' if op == 0xe9 else None
        if kind is None and a[p - 2] == 0xff and op in (0x15, 0x25):
            kind = 'icall' if op == 0x15 else 'ijmp'
        out.append('%s:%x' % (kind or 'ref', p - (1 if kind in ('call', 'jmp') else 2 if kind else 0)))
    print('%x (%d): %s' % (t, len(out), ' '.join(out)))
