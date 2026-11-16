# gdb -p <pid> -batch -x pestack.py  (env: TIDS=tid WBASE=hex) : for a thread that is inside Wine's unix
# side, find its TEB (gs base), read the PE stack bounds and print return-address candidates (RVAs) from
# the lowest (most recent) live part of the PE stack upward.
import gdb, os, struct
tids = [int(t) for t in os.environ['TIDS'].split(',')]
base = int(os.environ['WBASE'], 16)
gdb.execute('set pagination off')
inf = gdb.selected_inferior()
for th in inf.threads():
    if th.ptid[1] not in tids: continue
    th.switch()
    for reg in ('$gs_base', '$fs_base'):
        try:
            teb = int(gdb.parse_and_eval(reg))
        except gdb.error:
            continue
        if not teb: continue
        try:
            sb, sl = struct.unpack('<QQ', inf.read_memory(teb + 8, 16).tobytes())
        except gdb.error:
            continue
        print('tid %d %s=%x stack %x..%x' % (th.ptid[1], reg, teb, sl, sb))
        if not (0 < sl < sb and sb - sl < (64 << 20)): continue
        lo = max(sl, sb - (1 << 20))
        mem = inf.read_memory(lo, sb - lo).tobytes()
        q = struct.unpack('<%dQ' % (len(mem) // 8), mem)
        ra = [(lo + 8 * i, v - base) for i, v in enumerate(q) if base + 0x1000 <= v < base + 0x3500000]
        print('  first 40 return-address candidates (lowest addresses first):')
        print('  ' + ' '.join('%x' % r for _, r in ra[:40]))
gdb.execute('detach')
