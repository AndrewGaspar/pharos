# rtti.py <mangled-name-substring>... : MSVC RTTI lookup in the image dump: for each TypeDescriptor
# whose decorated name (".?AV...@@") contains the argument, list the vtables whose complete-object
# locator points at it (RVA of the vtable, offset of that subobject).
import re, struct, sys
import numpy as np
from img import m, base

u32 = np.frombuffer(m, dtype='<u4', count=len(m) // 4)
u64 = np.frombuffer(m, dtype='<u8', count=len(m) // 8)
for pat in sys.argv[1:]:
    for mt in re.finditer(rb'\.\?A[UV][^\0]*' + re.escape(pat.encode()) + rb'[^\0]*\0', m):
        name = mt.group(0)[:-1].decode(errors='replace')
        td = mt.start() - 0x10
        # COL: {sig=1, offset, cdOffset, typeDescriptor rva, classHierarchy rva, self rva}
        for i in np.nonzero(u32 == td)[0]:
            col = int(i) * 4 - 12
            if col < 0 or struct.unpack_from('<I', m, col)[0] != 1: continue
            if struct.unpack_from('<I', m, col + 20)[0] != col: continue
            off = struct.unpack_from('<I', m, col + 4)[0]
            for j in np.nonzero(u64 == base + col)[0]:
                print('%s: vtable rva %x (subobject offset %#x)' % (name, int(j) * 8 + 8, off))
