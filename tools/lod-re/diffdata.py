# diffdata.py <same1> <same2> <other> : 4-byte words in writable image sections that are equal in
# same1/same2 (same settings, different runs) but differ in <other> (changed setting).
import mmap, struct, sys
import numpy as np

def load(p):
    f = open(p, 'rb')
    return f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

fa, A = load(sys.argv[1]); fc, C = load(sys.argv[2]); fb, B = load(sys.argv[3])
pe = struct.unpack_from('<I', A, 0x3c)[0]
nsec = struct.unpack_from('<H', A, pe + 6)[0]
sopt = struct.unpack_from('<H', A, pe + 20)[0]
st = pe + 24 + sopt
secs = []
for i in range(nsec):
    name, vsize, va, rsize, rptr = struct.unpack_from('<8sIIII', A, st + 40 * i)
    chars = struct.unpack_from('<I', A, st + 40 * i + 36)[0]
    secs.append((name.rstrip(b'\0').decode(errors='replace'), va, vsize, chars))
for s in secs:
    print('section %-8s va %08x size %08x %s' % (s[0], s[1], s[2], 'W' if s[3] & 0x80000000 else ''))
out = []
for name, va, vsize, chars in secs:
    if not (chars & 0x80000000):
        continue
    n = vsize // 4
    a = np.frombuffer(A, dtype='<u4', count=n, offset=va)
    b = np.frombuffer(B, dtype='<u4', count=n, offset=va)
    c = np.frombuffer(C, dtype='<u4', count=n, offset=va)
    idx = np.nonzero((a == c) & (a != b))[0]
    for i in idx:
        rva = va + int(i) * 4
        va_, vb = int(a[i]), int(b[i])
        fa_, fb_ = struct.unpack('<f', struct.pack('<I', va_))[0], struct.unpack('<f', struct.pack('<I', vb))[0]
        out.append((rva, name, va_, vb, fa_, fb_))
print(len(out), 'candidates')
def plaus(x):
    return x == 0 or 1e-4 < abs(x) < 1e6
for rva, name, va_, vb, fa_, fb_ in out[:400]:
    tag = 'float' if plaus(fa_) and plaus(fb_) and (va_ > 0xffff or vb > 0xffff) else 'int'
    if tag == 'float':
        print('%08x %-6s same=%-12g other=%-12g' % (rva, name, fa_, fb_))
    else:
        print('%08x %-6s same=%-12d other=%-12d (0x%x/0x%x)' % (rva, name, va_, vb, va_, vb))
