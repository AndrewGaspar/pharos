# bulkcount.py : the BulkWorldComponent (World+0xe0 -> +0x1e8, vtable rva 0x36406c8): settings at
# +0xd0 (u32 8192, 160, 70, 40; float 0.69, 40, 1e4) and the counters at +0x190 / +0x1a0 / +0x208 /
# +0x260 / +0x270 (551 / 574 in Alexandria at k=1), plus the pool capacities at +0x100 / +0x148 /
# +0x1d0 / +0x218. Reads only that object.
import struct
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
B = L.q(L.q(W + 0xe0) + 0x1e8)
if not B or L.q(B) != L.base + 0x36406c8:
    print('BulkWorldComponent not found')
else:
    b = L.read(B, 0x350)
    u = lambda o: struct.unpack_from('<I', b, o)[0]
    f = lambda o: struct.unpack_from('<f', b, o)[0]
    print('settings %d %d %d %d %.2f %.0f %.0f | pools %d %d %d %d | counters +190 %d +1a0 %d +208 %d +260 %d +270 %d' % (
        u(0xd0), u(0xd4), u(0xd8), u(0xdc), f(0xe0), f(0xe4), f(0xe8),
        u(0x100), u(0x148), u(0x1d0), u(0x218), u(0x190), u(0x1a0), u(0x208), u(0x260), u(0x270)))
