# gdb -p <pid> -batch -x pestack.py  (env: WBASE=hex [TIDS=tid,tid; default all threads] [N=80]
# [IMG=image dump]) : PE-side call stacks of threads that sit in Wine's unix side (all threads of a
# frozen game). Per thread: TEB (gs base) -> PE stack bounds; the PE stack pointer at the time of the
# syscall is recovered from the unix-side stack (Wine's syscall frame keeps it: the lowest value on the
# first 1 MiB of the unix stack that lies inside the PE stack bounds); then the call-validated return
# addresses (bytes before them = E8 rel32 or an FF /2 call) from that rsp up to the stack base, as RVAs,
# innermost first. "live" = from the recovered rsp; if no rsp is found, "scan" = from the lowest
# address of the stack (includes stale data below the live frames; read with care).
# Reads memory only (no breakpoints, no writes); detaches at the end. IMG (the image dump, file offset
# = RVA) speeds up the call validation.
import gdb, os, struct
tids = [int(t) for t in os.environ.get('TIDS', '').split(',') if t]
base = int(os.environ['WBASE'], 16)
N = int(os.environ.get('N', '80'))
gdb.execute('set pagination off')
inf = gdb.selected_inferior()
img = None
if os.environ.get('IMG'):
    import mmap
    _fh = open(os.environ['IMG'], 'rb')
    img = mmap.mmap(_fh.fileno(), 0, access=mmap.ACCESS_READ)


def mem(a, n):
    try:
        return inf.read_memory(a, n).tobytes()
    except gdb.error:
        return b''


def code(a, n):
    if img is not None:
        r = a - base
        return img[r:r + n] if 0 <= r and r + n <= len(img) else b''
    return mem(a, n)


def is_call_ret(ra):
    b = code(ra - 7, 7)
    if len(b) != 7: return False
    if b[2] == 0xe8: return True                       # call rel32
    for ln in (2, 3, 4, 6, 7):                          # FF /2 (optionally REX-prefixed)
        i = 7 - ln
        if b[i] == 0xff and (b[i + 1] >> 3) & 7 == 2: return True
        if ln >= 3 and 0x40 <= b[i] <= 0x4f and b[i + 1] == 0xff and (b[i + 2] >> 3) & 7 == 2: return True
    return False


for th in inf.threads():
    if tids and th.ptid[1] not in tids: continue
    th.switch()
    name = th.name or ''
    try:
        teb = int(gdb.parse_and_eval('$gs_base'))
        ursp = int(gdb.parse_and_eval('$rsp'))
        upc = int(gdb.parse_and_eval('$pc'))
    except gdb.error:
        continue
    b = mem(teb + 8, 16)
    if not teb or len(b) != 16: continue
    sb, sl = struct.unpack('<QQ', b)
    if not (0 < sl < sb and sb - sl < (64 << 20)):
        print('tid %d %s: no PE stack (teb %x)' % (th.ptid[1], name, teb)); continue
    # PE rsp from the unix stack
    pe_rsp = None
    if not (sl <= ursp < sb):
        u = b''
        for off in range(0, 1 << 20, 0x10000):  # up to 1 MiB of the unix stack, until unmapped
            c = mem(ursp + off, 0x10000)
            if not c: break
            u += c
        uq = struct.unpack('<%dQ' % (len(u) // 8), u[:len(u) // 8 * 8])
        cands = [(v, i) for i, v in enumerate(uq) if sl <= v < sb and v % 8 == 0]
        if cands:
            pe_rsp, at = min(cands)
            if os.environ.get('FRAME'):  # the unix-stack qwords around the saved rsp (syscall frame: args)
                print('  frame @%x: %s' % (ursp + at * 8, ' '.join('%x' % v for v in uq[max(0, at - 20):at + 8])))
    else:
        pe_rsp = ursp  # the thread is executing PE code
    lo = pe_rsp if pe_rsp else max(sl, sb - (2 << 20))
    data = mem(lo, sb - lo)
    q = struct.unpack('<%dQ' % (len(data) // 8), data[:len(data) // 8 * 8])
    ra = [v - base for v in q if base + 0x1000 <= v < base + 0x3500000 and is_call_ret(v)]
    pcs = ('rva %x' % (upc - base)) if base <= upc < base + 0x10000000 else '%x' % upc
    print('tid %d %s teb %x stack %x..%x pc %s %s %x (%d frames)' % (
        th.ptid[1], name, teb, sl, sb, pcs, 'live rsp' if pe_rsp else 'scan from', lo, len(ra)))
    print('  ' + ' '.join('%x' % r for r in ra[:N]))
gdb.execute('detach')
