# vscale.py <vt rva> <off>[,<off>...] <factor> : multiply float fields in every live instance of a class
# (host-side write through /proc/<pid>/mem; test instances only). Remembers originals in
# vscale-<vt>.json so `vscale.py <vt> <offs> restore` puts them back.
import json, os, struct, sys
from live import Live

L = Live()
vt = int(sys.argv[1], 16)
offs = [int(o, 16) for o in sys.argv[2].split(',')]
arg = sys.argv[3]
state = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vscale-%x-%d.json' % (vt, L.pid))
orig = json.load(open(state)) if os.path.exists(state) else {}
w = open('/proc/%d/mem' % L.pid, 'r+b', buffering=0)
n = 0
for a in L.vt(vt):
    for o in offs:
        k = '%x' % (a + o)
        cur = L.f(a + o)
        if cur is None: continue
        if k not in orig: orig[k] = cur
        new = orig[k] if arg == 'restore' else orig[k] * float(arg)
        w.seek(a + o)
        w.write(struct.pack('<f', new))
        n += 1
json.dump(orig, open(state, 'w'))
print('wrote', n, 'fields')
