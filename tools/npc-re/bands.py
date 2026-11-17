# bands.py : MetaAI units of civilian spawners by distance band from the player: total, activated
# (+0x1b8 set) and spawned (+0x30 set: the unit's real objective object, which exists while its
# character is spawned). Walks the MetaAI manager's objective list; no heap scan.
import math, struct, collections
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
M = L.q(W + 0x1688)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
lst, n = L.q(M + 0x3b8), L.d(M + 0x3c0)
b = L.read(lst, n * 8) or b''
SV = L.base + 0x377d218
edges = [40, 80, 120, 200, 400]
names = ['0-40', '40-80', '80-120', '120-200', '200-400']
tot, act, spn = collections.Counter(), collections.Counter(), collections.Counter()
for i in range(len(b) // 8):
    sp = struct.unpack_from('<Q', b, 8 * i)[0]
    h = L.read(sp, 0xd0) if sp else None
    if not h or struct.unpack_from('<Q', h, 0)[0] != SV: continue
    arr, cnt = struct.unpack_from('<QI', h, 0xa0)
    cnt >>= 17
    if not arr or cnt > 4096: continue
    for u in struct.unpack('<%dQ' % cnt, L.read(arr, 8 * cnt) or b'\0' * 8 * cnt):
        uh = L.read(u, 0x1c0) if u else None
        if not uh: continue
        x, y = struct.unpack_from('<ff', uh, 0x10)
        d = math.hypot(x - px, y - py)
        for e, nm in zip(edges, names):
            if d <= e:
                tot[nm] += 1
                act[nm] += struct.unpack_from('<Q', uh, 0x1b8)[0] != 0
                spn[nm] += struct.unpack_from('<Q', uh, 0x30)[0] != 0
                break
print('band(m) units/active/spawned: ' + ' | '.join('%s %d/%d/%d' % (nm, tot[nm], act[nm], spn[nm]) for nm in names) +
      ' | spawned total %d' % sum(spn.values()))
