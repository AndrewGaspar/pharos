# optcmp.py <a.opts> <b.opts> : per option class, instance values (from +0x10) present in one run but not the other
import re, sys, collections
names = {}
for line in open('/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/lod/classes.txt'):
    p = line.split()
    if len(p) >= 7: names[p[6]] = p[0]
def parse(path):
    d = collections.defaultdict(list); cur = None
    for line in open(path):
        m = re.match(r'memtool: vt ([0-9a-f]+)', line)
        if m: cur = m.group(1); continue
        m = re.match(r'memtool:\s+([0-9a-f]+)\s(.*)', line)
        if m and cur:
            fields = [f for f in m.group(2).split() if int(f[1:].split(':')[0], 16) >= 0x10]
            d[cur].append(' '.join(fields))
    return d
A, B = parse(sys.argv[1]), parse(sys.argv[2])
for vt in sorted(set(A) | set(B)):
    ca, cb = collections.Counter(A[vt]), collections.Counter(B[vt])
    if ca == cb: continue
    print(f'== {names.get(vt, vt)} ({vt}): {sum(ca.values())} vs {sum(cb.values())} instances')
    for v in (ca - cb): print('   A only:', v)
    for v in (cb - ca): print('   B only:', v)
