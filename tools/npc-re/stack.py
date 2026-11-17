# stack.py [radius=60] : extra units vs their originals ("npc_units: extra X of Y" lines from
# [world] npc_units_log=1 in the instance log): for pairs where both are activated and within radius
# of the player, the distance between their positions (+0x10, which follows the spawned character).
# Pairs closer than 0.4 m are reported as stacked. No heap scan.
import math, os, re, struct, sys, collections
from live import Live

L = Live()
R = float(sys.argv[1]) if len(sys.argv) > 1 else 60
log = '/home/ajg/Games/aco-dlss-instances/%s/out/pharos.log' % os.environ['ACO_INST']
pairs = [(int(m[1], 16), int(m[2], 16)) for m in re.finditer(r'npc_units: extra ([0-9a-fA-F]+) of ([0-9a-fA-F]+)', open(log, errors='replace').read())]
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
UV = L.base + 0x377eab8
hist = collections.Counter()
stacked = []
n = 0
for e, o in pairs[-6000:]:
    he, ho = L.read(e, 0x1c0), L.read(o, 0x1c0)
    if not he or not ho or struct.unpack_from('<Q', he, 0)[0] != UV or struct.unpack_from('<Q', ho, 0)[0] != UV: continue
    if not struct.unpack_from('<Q', he, 0x1b8)[0] or not struct.unpack_from('<Q', ho, 0x1b8)[0]: continue
    xe, ye, ze = struct.unpack_from('<fff', he, 0x10)
    xo, yo, zo = struct.unpack_from('<fff', ho, 0x10)
    if math.hypot(xo - px, yo - py) > R: continue
    n += 1
    d = math.sqrt((xe - xo) ** 2 + (ye - yo) ** 2 + (ze - zo) ** 2)
    hist[min(int(d), 10)] += 1
    if d < 0.4: stacked.append((e, o, xo, yo, d))
print('%d active pairs within %g m of the player; distance histogram (m): %s' % (n, R, dict(sorted(hist.items()))))
for e, o, x, y, d in stacked[:20]:
    print('  stacked: extra %x original %x at (%.1f, %.1f), %.2f m apart' % (e, o, x, y, d))
