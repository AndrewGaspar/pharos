# dumpobj.py <addr hex> [bytes hex] : dump a live object as qwords/floats, image pointers as RVAs
import sys, struct
from live import Live

L = Live()
a = int(sys.argv[1], 16)
n = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x100
b = L.read(a, n)
for o in range(0, len(b), 8):
    v, = struct.unpack_from('<Q', b, o)
    lo, hi = struct.unpack_from('<II', b, o)
    f0, f1 = struct.unpack_from('<ff', b, o)
    note = ''
    if L.base <= v < L.base + 0x10000000: note = 'rva %x' % (v - L.base)
    def fmt(u, fl):
        if u == 0: return '0'
        if u < 0x100000: return 'i%d' % u
        if 1e-4 < abs(fl) < 1e7: return '%.4g' % fl
        return '%08x' % u
    print('%4x: %016x  %-12s %-12s %s' % (o, v, fmt(lo, f0), fmt(hi, f1), note))
