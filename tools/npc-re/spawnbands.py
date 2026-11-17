# spawnbands.py [--dump] : spawned characters by distance band from the player, from the spawning
# sources' candidate lists (SpawningManager = [World+0x13c0]+0x38; sources at +0x18 {ptr, n << 17};
# each source's candidates at +0xb8 {ptr, n << 17}, 0x18-byte entries with the candidate at +0x10).
# A candidate is spawned when its +0x30 object is set and that object's byte +0x33 == 0 (the
# game's check, RVA 0x1258fb0); its position is taken from that object (entity, +0x50).
import math, struct, sys, collections
from live import Live

L = Live()
W = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
SM = L.q(L.q(W + 0x13c0) + 0x38)
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
srcs, ns = L.q(SM + 0x18), L.d(SM + 0x20) >> 17
bands = [40, 80, 120, 200, 1e9]
names = ['0-40', '40-80', '80-120', '120-200', '200+']
tot = collections.Counter()
per_src = []
dump = '--dump' in sys.argv
for si in range(ns):
    s = L.q(srcs + 8 * si)
    if not s: continue
    arr, cnt = L.q(s + 0xb8), (L.d(s + 0xc0) or 0) >> 17
    sp = 0
    for ci in range(min(cnt, 20000)):
        e = L.read(arr + 0x18 * ci, 0x18)
        if not e: break
        cand = struct.unpack_from('<Q', e, 0x10)[0]
        ent = L.q(cand + 0x30) if cand else 0
        if not ent or (L.read(ent + 0x33, 1) or b'\1')[0] != 0: continue
        p = L.read(ent + 0x50, 8)
        if not p: continue
        x, y = struct.unpack('<ff', p)
        d = math.hypot(x - px, y - py)
        if dump and sp < 3: print('src %d cand %x ent %x vt %x pos %.1f %.1f d %.1f' % (si, cand, ent, (L.q(ent) or 0) - L.base, x, y, d))
        sp += 1
        for b, nme in zip(bands, names):
            if d <= b:
                tot[nme] += 1
                break
    per_src.append('%x:%d/%d' % ((L.q(s) or 0) - L.base, sp, cnt))
print('spawned by band: ' + ' '.join('%s m %d' % (n, tot[n]) for n in names) + ' | total %d | sources %s' % (sum(tot.values()), ' '.join(per_src)))
