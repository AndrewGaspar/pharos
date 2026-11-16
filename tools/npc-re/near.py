# near.py <x> <y> : MetaAI virtual units (vtable rva 0x377eab8 and other classes passed via VTS) by
# distance from (x, y): total and actually spawned (spawn request +0x1b8 -> +0x30 entity, RVA 0x1258fb0).
import os, struct, sys, math, collections
from live import Live

L = Live()
px, py = float(sys.argv[1]), float(sys.argv[2])
vts = [int(v, 16) for v in os.environ.get('VTS', '377eab8').split(',')]
found = L.scan_q([L.base + v for v in vts])
bands = [20, 40, 80, 120, 160, 250]
tot, sp = collections.Counter(), collections.Counter()
for vt, addrs in found.items():
    for a in addrs:
        b = L.read(a, 0x1c0)
        if not b: continue
        x, y = struct.unpack_from('<ff', b, 0x10)
        d = math.hypot(x - px, y - py)
        req = struct.unpack_from('<Q', b, 0x1b8)[0]
        s = False
        if req:  # spawned: request+0x30 set and its byte +0x33 == 0 (game check, RVA 0x1258fb0)
            e = L.q(req + 0x30)
            if e:
                bb = L.read(e + 0x33, 1)
                s = bool(bb) and bb[0] == 0
        for r in bands:
            if d <= r:
                tot[r] += 1
                sp[r] += s
print(' '.join('<=%dm: %d units, %d spawned |' % (r, tot[r], sp[r]) for r in bands))
