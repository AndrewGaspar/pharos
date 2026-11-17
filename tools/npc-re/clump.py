# clump.py [radius=80] : do extras still bunch up? From the "npc_units: extra X of U (... donor D ...)"
# lines ([world] npc_units_log=1) in the instance log: for activated extras within radius of the
# player, the distance to the nearest *sibling* (another extra made for the same original) and to the
# nearest other unit of the same archetype; plus how many distinct archetypes the extras near the
# player use. No heap scan (reads only the logged units).
import collections, math, os, re, struct, sys
from live import Live

L = Live()
R = float(sys.argv[1]) if len(sys.argv) > 1 else 80
log = '/home/ajg/Games/aco-dlss-instances/%s/out/aco_dlss.log' % os.environ['ACO_INST']
rows = [(int(m[1], 16), int(m[2], 16)) for m in re.finditer(r'npc_units: extra ([0-9a-fA-F]+) of ([0-9a-fA-F]+)', open(log, errors='replace').read())]
pl = L.q(L.q(L.base + 0x4c23c40))
px, py = struct.unpack('<ff', L.read(pl + 0x50, 8))
UV = L.base + 0x377eab8
live = {}
for e, o in rows:
    h = L.read(e, 0x1c0)
    if not h or struct.unpack_from('<Q', h, 0)[0] != UV or not struct.unpack_from('<Q', h, 0x1b8)[0]: continue
    x, y = struct.unpack_from('<ff', h, 0x10)
    if math.hypot(x - px, y - py) > R: continue
    live[e] = (x, y, o, struct.unpack_from('<Q', h, 0x38)[0])
sib = collections.defaultdict(list)
for e, (x, y, o, a) in live.items(): sib[o].append(e)
d_sib, d_arch = [], []
for e, (x, y, o, a) in live.items():
    ds = [math.hypot(x - live[f][0], y - live[f][1]) for f in sib[o] if f != e]
    if ds: d_sib.append(min(ds))
    da = [math.hypot(x - v[0], y - v[1]) for f, v in live.items() if f != e and v[3] == a]
    if da: d_arch.append(min(da))


def summ(v):
    if not v: return 'n/a'
    v = sorted(v)
    return 'n=%d, within 3 m: %d%%, median %.1f m' % (len(v), 100 * sum(1 for x in v if x < 3) // len(v), v[len(v) // 2])


print('%d activated extras within %g m; %d archetypes among them' % (len(live), R, len(set(v[3] for v in live.values()))))
print('nearest sibling (same original): ' + summ(d_sib))
print('nearest extra of the same archetype: ' + summ(d_arch))
