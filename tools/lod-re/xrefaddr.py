# xrefaddr.py <lo-rva> <hi-rva> : code locations with a RIP-relative disp32 pointing into [lo, hi)
import mmap, sys
import numpy as np
IMG = '/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/lod/envU-1.image'
f = open(IMG, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
CODE_END = 0x3538e00  # .data2 (code) section end
hits = []
for k in range(4):
    v = np.frombuffer(m, dtype='<i4', count=(CODE_END - k) // 4, offset=k).astype(np.int64)
    pos = np.arange(len(v), dtype=np.int64) * 4 + k
    tgt = pos + 4 + v
    for i in np.nonzero((tgt >= lo) & (tgt < hi))[0]:
        hits.append((int(pos[i]), int(tgt[i])))
for p, t in sorted(hits):
    print('%08x -> %08x' % (p, t))
