# gdb -p <pid> -batch -x hwbreak.py  (env: BADDR=<abs hex>[,<abs hex>...] BN=<hits> BEXPR="$rcx,*(long*)$rcx" WBASE=<hex>)
# Hardware execution breakpoint(s) (no code bytes are modified, so integrity checks don't see it):
# prints the given expressions for the first BN hits, then removes the breakpoints and detaches.
import gdb, os, time

addrs = [int(x, 16) for x in os.environ['BADDR'].split(',')]
n = int(os.environ.get('BN', '5'))
exprs = os.environ.get('BEXPR', '$rcx').split(',')
base = int(os.environ.get('WBASE', '0'), 16)
deadline = time.time() + float(os.environ.get('WSECS', '20'))

gdb.execute('set pagination off')
gdb.execute('set confirm off')
for sig in ['SIGSEGV', 'SIGUSR1', 'SIGUSR2', 'SIGILL', 'SIGBUS', 'SIGFPE', 'SIGALRM', 'SIGCHLD', 'SIGPIPE', 'SIGIO',
            'SIGPROF', 'SIGWINCH', 'SIGQUIT', 'SIGABRT', 'SIGSYS', 'SIG32', 'SIG33', 'SIG34']:
    try:
        gdb.execute('handle %s nostop noprint pass' % sig, to_string=True)
    except gdb.error:
        pass
for a in addrs:
    gdb.execute('hbreak *0x%x' % a, to_string=True)


def rel(v):
    v &= 0xffffffffffffffff
    return 'rva %x' % (v - base) if base and base <= v < base + 0x10000000 else '%x' % v


for i in range(n):
    if time.time() > deadline:
        break
    try:
        gdb.execute('continue', to_string=True)
    except (gdb.error, KeyboardInterrupt) as e:
        print('stopped:', e)
        break
    if time.time() > deadline:
        break
    pc = int(gdb.selected_frame().pc())
    vals = []
    for e in exprs:
        try:
            vals.append('%s=%s' % (e, rel(int(gdb.parse_and_eval(e)))))
        except gdb.error as err:
            vals.append('%s=?' % e)
    print('hit %s tid=%d %s' % (rel(pc), gdb.selected_thread().ptid[1], ' '.join(vals)))
    nst = int(os.environ.get('BSTACK', '0'))
    if nst:
        # Return-address candidates: stack qwords pointing into the exe's code (.text below rva 0x3500000).
        rsp = int(gdb.parse_and_eval('$rsp'))
        mem = gdb.selected_inferior().read_memory(rsp, 8 * 400).tobytes()
        import struct
        ra = [v - base for v in struct.unpack('<400Q', mem) if base + 0x1000 <= v < base + 0x3500000]
        print('   stack:', ' '.join('%x' % r for r in ra[:nst]))
gdb.execute('delete', to_string=True)
gdb.execute('detach')
