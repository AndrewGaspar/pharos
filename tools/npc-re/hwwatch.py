# gdb -p <pid> -batch -x hwwatch.py  (env: WADDR=<hex> WLEN=<1|2|4|8> WKIND=awatch|rwatch|watch WN=<hits> WBASE=<image base hex>)
# Hardware watchpoint on a live game process: records the instruction (as an image RVA when inside
# the exe) and the return-address guess for the first WN hits, then detaches.
import gdb, os, collections, time

addrs = [int(x, 16) for x in os.environ['WADDR'].split(',')]  # up to 4 (debug registers)
addr = addrs[0]
ln = int(os.environ.get('WLEN', '4'))
kind = os.environ.get('WKIND', 'awatch')
n = int(os.environ.get('WN', '50'))
base = int(os.environ.get('WBASE', '0'), 16)
deadline = time.time() + float(os.environ.get('WSECS', '20'))

gdb.execute('set pagination off')
gdb.execute('set confirm off')
for sig in ['SIGSEGV', 'SIGUSR1', 'SIGUSR2', 'SIGILL', 'SIGBUS', 'SIGFPE', 'SIGALRM', 'SIGCHLD', 'SIGPIPE', 'SIGIO',
            'SIGPROF', 'SIGWINCH', 'SIGQUIT', 'SIGABRT', 'SIGSYS', 'SIGRTMIN', 'SIG32', 'SIG33', 'SIG34']:
    try:
        gdb.execute('handle %s nostop noprint pass' % sig, to_string=True)
    except gdb.error:
        pass
done = [False]  # gw.sh sends SIGINT to gdb at the deadline, which interrupts 'continue'
ctype = {1: 'char', 2: 'short', 4: 'int', 8: 'long'}[ln]
for a_ in addrs:
    gdb.execute('%s *(%s*)0x%x' % (kind, ctype, a_), to_string=True)

def rel(v):
    return 'rva %x' % (v - base) if base and base <= v < base + 0x10000000 else '%x' % v

hits = collections.Counter()
rets = {}
for i in range(n):
    if done[0] or time.time() > deadline:
        break
    try:
        gdb.execute('continue', to_string=True)
    except (gdb.error, KeyboardInterrupt) as e:
        print('stopped:', e)
        break
    if time.time() > deadline:
        break
    fr = gdb.selected_frame()
    pc = int(fr.pc())
    rsp = int(gdb.parse_and_eval('$rsp'))
    try:
        ret = int(gdb.parse_and_eval('*(long*)%d' % rsp)) & 0xffffffffffffffff
    except gdb.error:
        ret = 0
    val = ','.join(str(int(gdb.parse_and_eval('*(%s*)0x%x' % (ctype, a_)))) for a_ in addrs)
    hits[pc] += 1
    rets.setdefault(pc, set()).add(ret)
    if hits[pc] <= int(os.environ.get('WPER', '1')):
        print('hit %s val=%s tid=%d [rsp]=%s' % (rel(pc), val, gdb.selected_thread().ptid[1], rel(ret)))
        nst = int(os.environ.get('BSTACK', '0'))
        if nst:
            import struct
            mem = gdb.selected_inferior().read_memory(rsp, 8 * 400).tobytes()
            ra = [v - base for v in struct.unpack('<400Q', mem) if base + 0x1000 <= v < base + 0x3500000]
            print('   stack:', ' '.join('%x' % r for r in ra[:nst]))
gdb.execute('delete', to_string=True)
for pc, c in hits.most_common():
    print('%-14s n=%d callers %s' % (rel(pc), c, ' '.join(rel(r) for r in list(rets[pc])[:4])))
gdb.execute('detach')
