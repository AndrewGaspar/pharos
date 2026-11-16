# ti.py <rva> [bytes] : dump qwords at an RVA, annotating image pointers (and strings they point to)
import sys
from img import *

r = int(sys.argv[1], 16)
n = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x100
for o in range(0, n, 8):
    v = q(r + o)
    x = ''
    if is_img(v): x = '-> rva %x %s' % (v - base, cstr(v) or '')
    lo, hi = v & 0xffffffff, v >> 32
    print('%4x: %016x  i%d/%d f%g/%g %s' % (o, v, lo, hi, f(r + o), f(r + o + 4), x))
