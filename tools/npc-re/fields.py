# fields.py <vt rva hex> <size hex> : per-offset value distribution across all live instances of a class
# (dwords; floats shown as floats). Helps spot counts / radii / probabilities in data objects.
import collections, struct, sys
from live import Live

L = Live()
vt, size = int(sys.argv[1], 16), int(sys.argv[2], 16)
rows = [struct.unpack('<%dI' % (size // 4), L.read(a, size)) for a in L.vt(vt)]
print(len(rows), 'instances')


def fmt(u):
    f = struct.unpack('<f', struct.pack('<I', u))[0]
    if u == 0: return '0'
    if u < 0x100000: return 'i%d' % u
    if 1e-4 < abs(f) < 1e7: return '%.4g' % f
    return '%x' % u


for i in range(2, size // 4):
    c = collections.Counter(x[i] for x in rows)
    ptrish = sum(n for v, n in c.items() if v > 0x10000000 and not (1e-4 < abs(struct.unpack('<f', struct.pack('<I', v))[0]) < 1e7))
    if len(c) <= 10:
        print('+%-4x' % (i * 4), ' '.join('%s:%d' % (fmt(v), n) for v, n in c.most_common(10)))
    else:
        print('+%-4x' % (i * 4), 'distinct=%d' % len(c), ' '.join('%s:%d' % (fmt(v), n) for v, n in c.most_common(6)))
