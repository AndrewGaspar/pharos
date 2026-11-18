# vtname.py <vt rva>... : class name for a vtable: find code that installs it (lea vt(%rip) in a
# ctor), then the TypeInfo whose ctor field (+0x40) points at that ctor or at a thunk jumping to it.
import re, struct, subprocess, sys
import numpy as np
from img import m, base, q, cstr, IMG

CODE_END = 0x3400000
a8 = np.frombuffer(m, dtype=np.uint8, count=CODE_END)


def lea_sites(t):
    out = []
    for k in range(4):
        v = np.frombuffer(m, dtype='<i4', count=(CODE_END - k) // 4, offset=k).astype(np.int64)
        pos = np.arange(len(v), dtype=np.int64) * 4 + k
        for i in np.nonzero(v + pos + 4 == t)[0]:
            p = int(pos[i])
            if a8[p - 2] == 0x8d: out.append(p - 3)
    return out


def func_start(addr):
    # walk back to the previous int3 padding / ret
    p = addr
    while p > 0 and not (m[p - 1] == 0xcc or (m[p - 1] == 0xc3 and m[p - 2] in (0xcc, 0x5b, 0x5d, 0xc3))):
        p -= 1
        if addr - p > 0x400: break
    while m[p] == 0xcc: p += 1
    return p


tis = np.frombuffer(m[0x4100000:0x4800000][:(0x700000 // 8) * 8], dtype='<u8')
for arg in sys.argv[1:]:
    vt = int(arg, 16)
    names = set()
    for s in lea_sites(vt):
        f = func_start(s)
        targets = {base + f}
        # thunks that jmp to f
        for k in range(4):
            v = np.frombuffer(m, dtype='<i4', count=(CODE_END - k) // 4, offset=k).astype(np.int64)
            pos = np.arange(len(v), dtype=np.int64) * 4 + k
            for i in np.nonzero(v + pos + 4 == f)[0]:
                p = int(pos[i])
                if a8[p - 1] in (0xe9, 0xe8): targets.add(base + p - 1)
        for t in targets:
            for i in np.nonzero(tis == t)[0]:
                r = 0x4100000 + int(i) * 8
                nm = cstr(q(r - 0x40))
                if nm: names.add(nm)
    print('%x: %s' % (vt, ', '.join(sorted(names)) or '?'))
