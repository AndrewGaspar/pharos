# fstart.py <rva>... : start of the function containing each RVA (walks back to the int3 / padding
# that MSVC puts between functions; heuristic, like decomp.sh @rva).
import sys
from img import m

for x in sys.argv[1:]:
    r = int(x, 16)
    p = r
    while p > 0 and not (m[p - 1] == 0xcc and m[p - 2] == 0xcc) and r - p < 0x8000:
        p -= 1
    print('%x %x' % (r, p))
