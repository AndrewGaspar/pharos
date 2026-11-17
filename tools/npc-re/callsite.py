# callsite.py <return-rva>... : for each return address, the call instruction just before it (target
# for direct calls) and the start of the containing function (int3-padding heuristic).
import subprocess, sys
from img import m, IMG


def fstart(r):
    p = r
    while p > 0 and not (m[p - 1] == 0xcc and m[p - 2] == 0xcc) and r - p < 0x8000: p -= 1
    return p


for x in sys.argv[1:]:
    r = int(x, 16)
    d = subprocess.run(['x86_64-w64-mingw32-objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64',
                        '--start-address=%d' % (r - 7), '--stop-address=%d' % r, IMG],
                       capture_output=True, text=True).stdout.splitlines()[7:]
    ins = [l.split('\t')[-1].strip() for l in d if '\t' in l and 'call' in l]
    print('%x: in fn %x  %s' % (r, fstart(r), ins[-1] if ins else '?'))
