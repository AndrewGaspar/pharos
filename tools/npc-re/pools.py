# pools.py : the SpawningManager's pool list (+0x58 -> {entries, n << 17}, 0x28-byte entries; the
# spawned count is the sum of the u32 at +0x10) and the second list it counts (+0x6d, 16-byte
# entries, +8 == 1 counts), dumped raw, plus the spawning sources (+0x18).
import struct
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
SM = L.q(L.q(W + 0x13c0) + 0x38)
print('SpawningManager %x vt %x' % (SM, L.q(SM) - L.base))
pl = L.q(SM + 0x58)
arr, cnt = L.q(pl), L.d(pl + 8) >> 17
print('pool list %x: %d entries' % (arr, cnt))
tot = 0
for i in range(cnt):
    e = L.read(arr + 0x28 * i, 0x28)
    q = struct.unpack('<5Q', e)
    d = struct.unpack('<10I', e)
    tot += d[4]
    o = q[0]
    vt = (L.q(o) - L.base) if o and L.q(o) else 0
    print('  [%d] %s | obj vt %x | u32s %s' % (i, ' '.join('%016x' % v for v in q), vt, d))
print('sum +0x10 =', tot)
a2, n2 = L.q(SM + 0x6d), L.d(SM + 0x75) >> 17
print('list +0x6d: %d entries, +8==1: %d' % (n2, sum(1 for i in range(n2) if (L.read(a2 + 16 * i + 8, 1) or b'\0')[0] == 1)))
