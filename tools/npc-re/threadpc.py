# gdb -p <pid> -batch -x threadpc.py  (env: TIDS=tid,tid  WBASE=hex) : print pc + stack return-address
# candidates (as RVAs) for the given threads, sampled 5 times, then detach.
import gdb, os, struct
tids = [int(t) for t in os.environ['TIDS'].split(',')]
base = int(os.environ['WBASE'], 16)
gdb.execute('set pagination off')
def rel(v): return 'rva %x' % (v - base) if base <= v < base + 0x10000000 else '%x' % v
for sample in range(1):
    for th in gdb.selected_inferior().threads():
        if th.ptid[1] not in tids: continue
        th.switch()
        pc = int(gdb.selected_frame().pc())
        rsp = int(gdb.parse_and_eval('$rsp'))
        mem = gdb.selected_inferior().read_memory(rsp, 8 * 300).tobytes()
        ra = [v - base for v in struct.unpack('<300Q', mem) if base + 0x1000 <= v < base + 0x3500000]
        print('tid %d pc %s stack %s' % (th.ptid[1], rel(pc), ' '.join('%x' % r for r in ra[:14])))
gdb.execute('detach')
