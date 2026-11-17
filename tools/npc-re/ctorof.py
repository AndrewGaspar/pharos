# ctorof.py <rva>... : which Anvil TypeInfo records use the function as constructor (TypeInfo: name
# pointer at +0, size at +0x10, ctor at +0x40), plus every other image qword pointing at it
# (vtables, function tables).
import sys
import numpy as np
import json, os
from img import m, base, q, cstr

a = np.frombuffer(m, dtype='<u8', count=len(m) // 8)
names = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vtnames.json')))
for x in sys.argv[1:]:
    r = int(x, 16)
    out = []
    for i in np.nonzero(a == base + r)[0]:
        p = int(i) * 8
        ti = p - 0x40
        nm = cstr(q(ti)) if ti >= 0 else None
        if nm:
            out.append('TypeInfo %x %s size %#x' % (ti, nm, q(ti + 0x10)))
        else:
            out.append('ptr at %x' % p)
    print('%x: %s' % (r, '; '.join(out) or '-'))
