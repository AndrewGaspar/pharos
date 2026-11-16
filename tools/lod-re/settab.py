# settab.py <image>... : graphics setting descriptor table at 0x49af058 (0x2c-byte entries)
import mmap, struct, sys
for p in sys.argv[1:]:
    f = open(p, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    print('==', p.split('/')[-1])
    r = 0x49af058 - 0x2c * 2
    for k in range(40):
        e = r + k * 0x2c
        a, b = struct.unpack_from('<ii', m, e)
        flag = m[e + 8]
        name = m[e + 9:e + 0x2c].split(b'\0')[0].decode(errors='replace')
        raw = m[e:e + 0x2c].hex()
        print('%08x levels=%-3d default=%-3d flag=%d %-20s' % (e, a, b, flag, name))
