# ctx.py <rva> <n_words> <image>... : show 32-bit words around rva in each image (float + hex)
import mmap, struct, sys
rva = int(sys.argv[1], 16); n = int(sys.argv[2])
ms = []
for p in sys.argv[3:]:
    f = open(p, 'rb'); ms.append((p.split('/')[-1], f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)))
start = rva - (n // 2) * 4
for i in range(n):
    r = start + 4 * i
    cols = []
    for name, f, m in ms:
        u = struct.unpack_from('<I', m, r)[0]; fl = struct.unpack_from('<f', m, r)[0]
        cols.append(('%08x %-11.5g' % (u, fl)) if u > 0xffff else ('%08x i%-10d' % (u, u)))
    print('%08x%s %s' % (r, ' *' if r == rva else '  ', ' | '.join(cols)))
