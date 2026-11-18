# perfhot.py <perf.data> <pid> [top=25] : flat profile of a `perf record -e cpu-clock:u -p <pid>`
# capture of the game, per thread (by name) and per module; samples inside ACOrigins.exe are grouped
# by function start (fstart.py's heuristic on the image dump) and printed as RVAs. Needs the game
# still running (reads /proc/<pid>/maps and task names).
import collections, os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
data, pid = sys.argv[1], int(sys.argv[2])
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
maps = []
for line in open('/proc/%d/maps' % pid):
    mm = re.match(r'([0-9a-f]+)-([0-9a-f]+) \S+ ([0-9a-f]+) \S+ \S+\s*(.*)', line)
    maps.append((int(mm[1], 16), int(mm[2], 16), int(mm[3], 16), mm[4]))
maps.sort()
base = min(a for a, b, off, n in maps if n.endswith('ACOrigins.exe'))
names = {}
for t in os.listdir('/proc/%d/task' % pid):
    try: names[int(t)] = open('/proc/%d/task/%s/comm' % (pid, t)).read().strip()
    except OSError: pass
import bisect
starts = [m[0] for m in maps]
def module(ip):
    if base <= ip < base + 0x6000000: return 'ACOrigins.exe'
    i = bisect.bisect_right(starts, ip) - 1
    if i >= 0 and maps[i][0] <= ip < maps[i][1]:
        return os.path.basename(maps[i][3]) or '[anon]'
    return '?'
out = subprocess.run(['perf', 'script', '-i', data, '-F', 'tid,ip'], capture_output=True, text=True).stdout
by_thread = collections.Counter(); by_mod = collections.Counter(); rvas = collections.Counter()
n = 0
for line in out.splitlines():
    p = line.split()
    if len(p) < 2: continue
    tid, ip = int(p[0]), int(p[1], 16)
    n += 1
    tn = names.get(tid, str(tid))
    by_thread[tn] += 1
    m = module(ip)
    by_mod[(tn, m)] += 1
    if m == 'ACOrigins.exe': rvas[ip - base] += 1
print('%d samples' % n)
print('threads:', ', '.join('%s %d' % kv for kv in by_thread.most_common(12)))
print('thread / module:')
for (tn, m), c in by_mod.most_common(top): print('  %6d  %-18s %s' % (c, tn, m))
# group game RVAs by function start
from img import m as IMG  # noqa
def fstart(r):
    p = r
    while p > 0 and not (IMG[p - 1] == 0xcc and IMG[p - 2] == 0xcc) and r - p < 0x8000:
        p -= 1
    return p
funcs = collections.Counter()
cache = {}
for rva, c in rvas.items():
    k = rva & ~0xff
    if k not in cache: cache[k] = None
for rva, c in rvas.items():
    f = cache.get(rva)
    if f is None:
        try: f = fstart(rva)
        except Exception: f = rva
        cache[rva] = f
    funcs[f] += c
print('ACOrigins.exe functions:')
for f, c in funcs.most_common(top): print('  %6d  %.1f%%  0x%x' % (c, 100.0 * c / max(n, 1), f))
