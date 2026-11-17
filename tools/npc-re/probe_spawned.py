# probe_spawned.py : for activated units within 40 m, dump the activation record (+0x1b8) and the
# object at its +0x28, to find which field marks a spawned character (candidate +0x30 -> entity,
# spawned when that entity's byte +0x33 == 0: the game's check at RVA 0x1258fb0).
import math, struct, collections
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
lst, n = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, n * 8)
SV = L.base + 0x377d218
c = collections.Counter()
shown = 0
for i in range(n):
    sp = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(sp, 0xd0) if sp else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != SV: continue
    arr, cnt = struct.unpack_from('<QI', h, 0xa0)
    cnt >>= 17
    if not arr or cnt > 4096: continue
    for u in struct.unpack('<%dQ' % cnt, L.read(arr, 8 * cnt)):
        uh = L.read(u, 0x1c0)
        x, y = struct.unpack_from('<ff', uh, 0x10)
        if math.hypot(x - px, y - py) > 40: continue
        req = struct.unpack_from('<Q', uh, 0x1b8)[0]
        if not req:
            c['inactive'] += 1
            continue
        r = L.read(req, 0x60)
        vals = [struct.unpack_from('<Q', r, o)[0] for o in range(0, 0x60, 8)]
        cand = vals[5]
        ch = L.read(cand, 0x40) if cand else None
        e = struct.unpack_from('<Q', ch, 0x30)[0] if ch else 0
        s = bool(e) and (L.read(e + 0x33, 1) or b'\1')[0] == 0
        c['spawned' if s else 'not'] += 1
        if shown < 4:
            print('req', ' '.join('%x' % v for v in vals))
            if ch: print('  +28 obj', ' '.join('%x' % struct.unpack_from('<Q', ch, o)[0] for o in range(0, 0x40, 8)))
            shown += 1
print(dict(c))
