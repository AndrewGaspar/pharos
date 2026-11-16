# fieldrefs.py <lo rva> <hi rva> <ptr off hex> : find code in [lo,hi) that loads a pointer from [r+ptr_off]
# and within the next 12 instructions dereferences it at some displacement. Prints the field offsets used.
import re, struct, sys
import capstone
from img import *

lo, hi, poff = int(sys.argv[1], 16), int(sys.argv[2], 16), int(sys.argv[3], 16)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.skipdata = True
b = bytes(m[lo:hi])
ins = list(md.disasm(b, lo))
pat = re.compile(r'(\w+), qword ptr \[(\w+) \+ 0x%x\]$' % poff)
for i, x in enumerate(ins):
    if x.mnemonic != 'mov': continue
    mt = pat.match(x.op_str)
    if not mt: continue
    reg = mt[1]
    uses = []
    for y in ins[i + 1:i + 13]:
        mm = re.search(r'\[%s \+ (0x[0-9a-f]+)\]' % reg, y.op_str)
        if mm: uses.append('%s %s' % (y.mnemonic, mm[1]))
        if re.match(r'%s,' % reg, y.op_str) and y.mnemonic in ('mov', 'lea', 'xor', 'pop'): break
    if uses: print('%x: %s' % (x.address, ', '.join(uses)))
