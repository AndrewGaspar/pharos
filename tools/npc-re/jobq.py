# jobq.py : host-side read of the engine task scheduler (RVA 0x4873e38) of the running/frozen game
# (ACO_INST=t1|t3): the per-frame root job (RVA 0x4873c18, waited on by RVA 0x161de10 / 0x1605640
# through its event at +0x1e0), the three global job queues (scheduler + i * 0x238: lock at +0,
# job array {ptr +0x28, u32 count +0x30, capacity +0x34}) and each task thread's three queues
# (thread objects in the Anvil array at scheduler +0x6a8; queues at thread + 0xd8 + i * 0x238).
# Job fields (see RVAs 0x8c8b50 enqueue, 0x8cc4b0 dequeue, 0x8d46c0 run, 0x8cf600 start):
# +0x1a0 name, +0x30/+0x34 thread masks, +0x38 queue index, +0x40 width, +0x44 taken, +0x48 runs,
# +0x4c state (0 idle, 1 queued, 2 done/dequeued), +0x54 starts, +0x58 pending, +0x148/+0x150
# dependency (checked by the callback at +0x168), +0x198 front flag, +0x1bc/+0x1bd/+0x1dd flags.
# Reads only; a few KB.
import json, os
from live import Live

L = Live()
B = L.base
S = B + 0x4873e38
names = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vtnames.json')))


def D(a):  # signed u32 read, -1 when unreadable
    v = L.d(a)
    return -1 if v is None else (v - (1 << 32) if v >= 1 << 31 else v)


def b1(a):
    b = L.read(a, 1)
    return b[0] if b else -1


def cs(p, n=64):
    b = L.read(p, n) if p else None
    if not b: return None
    b = b.split(b'\0')[0]
    return b.decode(errors='replace') if b and all(32 <= c < 127 for c in b) else None


def job(p, tag=''):
    vt = L.q(p) or 0
    vn = names.get('%x' % (vt - B), 'vt%x' % (vt - B)) if B <= vt < B + 0x10000000 else '%x' % vt
    d = lambda o: D(p + o)
    if not p: return
    print('%s job %x %-40s %s mask %x/%x q %d width %d taken %d runs %d state %d starts %d pending %d dep %x/%x '
          'cb %x front %d flags %d/%d/%d' % (
              tag, p, cs(L.q(p + 0x1a0)) or cs(L.q(p + 0x1b0)) or '?', vn, d(0x30), d(0x34), d(0x38), d(0x40),
              d(0x44), d(0x48), d(0x4c), d(0x54), d(0x58), L.q(p + 0x148) or 0, L.q(p + 0x150) or 0,
              (L.q(p + 0x168) or B) - B, b1(p + 0x198), b1(p + 0x1bc), b1(p + 0x1bd),
              b1(p + 0x1dd)))


def queue(qa, tag):
    arr, n, cap = L.q(qa + 0x28), D(qa + 0x30), D(qa + 0x34)
    print('%s queue %x: %d jobs (cap %x)' % (tag, qa, n, cap))
    for i in range(min(n or 0, 64)):
        job(L.q(arr + 8 * i), '   ')


job(B + 0x4873c18, 'frame root')
for i in range(3):
    queue(S + i * 0x238, 'global %d' % i)
ta, tn = L.q(S + 0x6a8), (L.d(S + 0x6b0) or 0) >> 17
print('scheduler counters +8b8 %d +8c0 %d +8c4 %d, %d threads' % (D(S + 0x8b8), D(S + 0x8c0), D(S + 0x8c4), tn))
for t in range(tn):
    th = L.q(ta + 8 * t)
    print('thread %d %x: running %d sleep-count %d' % (t, th, b1(th + 0x780), D(th + 0x788)))
    for i in range(3):
        qa = th + 0xd8 + i * 0x238
        if L.d(qa + 0x30): queue(qa, '  t%d/%d' % (t, i))

# The job graph: dependents in the Anvil array {+0x70 ptr, +0x78 n << 17} (prerequisites at +0xc0);
# when a job completes, each dependent's +0x58 (pending prerequisites) is decremented and the
# dependent is started at 0 (RVA 0x8caf10). Walk it from the frame root and print every job that is
# not idle (state != 0, or pending prerequisites, or threads still inside it).
r = B + 0x4873c18
print('frame root: +1cc %d +1d0 %d running %d +218 %d' % (D(r + 0x1cc), D(r + 0x1d0), b1(r + 0x1dc),
                                                       b1(r + 0x218)))
eng = L.q(B + 0x4c298b8)  # engine frame graph object (built by RVA 0x1617880, started by 0x15f88b0)
print('engine graph object %x: +0 %d +21e %d +3f4 %d' % (eng or 0, b1(eng), b1(eng + 0x21e),
                                                       b1(eng + 0x3f4)) if eng else 'no engine graph')
if eng: job(eng + 0x238, 'engine root')
seen, order, todo = set(), [], [r] + ([eng + 0x238] if eng else [])
while todo and len(seen) < 6000:
    p = todo.pop(0)
    if p in seen or not p: continue
    seen.add(p); order.append(p)
    arr, n = L.q(p + 0x70), (L.d(p + 0x78) or 0) >> 17  # +0x60 (armed flag) is cleared after notifying
    for i in range(min(n, 512)):
        todo.append(L.q(arr + 8 * i))
print('graph: %d jobs reachable from the frame root' % len(order))
for p in order:
    if D(p + 0x4c) or D(p + 0x58) or D(p + 0x44):
        job(p, '  busy')
        if D(p + 0x58) > 0:  # waiting for prerequisites: list them (Anvil array at +0xc0, see RVA 0x8c84c0)
            arr, n = L.q(p + 0xc0), (L.d(p + 0xc8) or 0) >> 17
            for i in range(min(n, 32)):
                q0 = L.q(arr + 8 * i)  # points at the prerequisite's +0x60
                if q0: job(q0 - 0x60, '      prereq')
