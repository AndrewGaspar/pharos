# filt.py <same1> <same2> <other> [more same-run images...] : float globals that differ meaningfully
import mmap, struct, sys
import numpy as np
imgs = []
for p in sys.argv[1:]:
    f = open(p, 'rb'); imgs.append((f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)))
A = imgs[0][1]; C = imgs[1][1]; B = imgs[2][1]
pe = struct.unpack_from('<I', A, 0x3c)[0]; nsec = struct.unpack_from('<H', A, pe + 6)[0]
sopt = struct.unpack_from('<H', A, pe + 20)[0]; st = pe + 24 + sopt
for i in range(nsec):
    name, vsize, va, rsize, rptr = struct.unpack_from('<8sIIII', A, st + 40 * i)
    chars = struct.unpack_from('<I', A, st + 40 * i + 36)[0]
    if not chars & 0x80000000: continue
    n = vsize // 4
    a = np.frombuffer(A, dtype='<f4', count=n, offset=va); b = np.frombuffer(B, dtype='<f4', count=n, offset=va)
    c = np.frombuffer(C, dtype='<f4', count=n, offset=va)
    ai = a.view('<u4'); bi = b.view('<u4'); ci = c.view('<u4')
    with np.errstate(all='ignore'):
        ok = (ai == ci) & (ai != bi)
        pl = lambda x: (x == 0) | ((np.abs(x) > 1e-3) & (np.abs(x) < 1e6))
        ok &= pl(a) & pl(b) & ((ai > 0xffff) | (ai == 0)) & ((bi > 0xffff) | (bi == 0)) & ~((ai == 0) & (bi == 0))
        rel = np.abs(a - b) / np.maximum(np.maximum(np.abs(a), np.abs(b)), 1e-9)
        ok &= rel > 0.05
    for j in np.nonzero(ok)[0]:
        print('%08x same=%-10g other=%-10g  ratio=%.3f' % (va + 4 * int(j), a[j], b[j], (a[j] / b[j]) if b[j] else float('inf')))
