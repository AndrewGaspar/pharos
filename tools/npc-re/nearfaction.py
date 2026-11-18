# nearfaction.py <faction id hex> [n=5] : the nearest activated units of a faction (by the faction
# data object's id), with heading from the player, so the camera can be pointed at one for a look.
import math, struct, sys
from live import Live

L = Live()
want = int(sys.argv[1], 16)
N = int(sys.argv[2]) if len(sys.argv) > 2 else 5
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
lst, n = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, n * 8)
SV = L.base + 0x377d218


def obj_of_link(link):
    if not link: return 0
    o, f = L.q(link), L.q(link + 8)
    return o if (f is not None and (f >> 31) & 1) else 0


rows = []
for i in range(n):
    s = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(s, 0xd0) if s else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != SV: continue
    a, c = struct.unpack_from('<QI', h, 0xa0)
    c >>= 17
    if not a or c > 16384: continue
    for u in struct.unpack('<%dQ' % c, L.read(a, 8 * c) or b'\0' * 8 * c):
        uh = L.read(u, 0x1c0) if u else None
        if not uh or not struct.unpack_from('<Q', uh, 0x1b8)[0]: continue
        ar = obj_of_link(struct.unpack_from('<Q', uh, 0x38)[0])
        fl = L.q(ar + 0x10) if ar else 0
        if not fl or L.q(fl + 0x10) != want: continue
        x, y = struct.unpack_from('<ff', uh, 0x10)
        rows.append((math.hypot(x - px, y - py), u, x, y, ar))
rows.sort()
for d, u, x, y, ar in rows[:N]:
    print('%.1f m yaw %.0f unit %x archetype %x at %.1f %.1f' % (d, math.degrees(math.atan2(y - py, x - px)), u, ar, x, y))
