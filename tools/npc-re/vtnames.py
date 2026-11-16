# vtnames.py : build vtable RVA -> class name map from Anvil TypeInfo records in the image dump.
# TypeInfo: +0 name ptr, +0x10 size, +0x40 ctor. A class's vtable is the last `lea reg,[rip+vt]`
# that is then stored to [reg] (offset 0) in its ctor, before the first ret/tail jump.
# Needs capstone (run with the scratch venv python). Writes vtnames.json: {"<vt rva hex>": "Class"}.
import json, os, re, struct
import numpy as np
import capstone
from img import *

VT_LO, VT_HI = 0x3500000, 0x3c00000
TEXT_LO, TEXT_HI = 0x1000, 0x3500000
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

b = bytes(m)
q64 = np.frombuffer(b[:len(b) // 8 * 8], dtype='<u8')
cand = np.nonzero((q64 >= base + 0x3500000) & (q64 < base + 0x3c00000))[0]
out = {}
for i in cand:
    if int(i) + 9 >= len(q64): continue
    ctor = int(q64[i + 8])
    if not (base + TEXT_LO <= ctor < base + TEXT_HI): continue
    nm = cstr(int(q64[i]))
    if not nm or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_:]{2,80}', nm): continue
    c = ctor - base
    regs = {}
    own = None
    for ins in md.disasm(b[c:c + 0x600], c):
        if ins.mnemonic == 'lea' and '[rip' in ins.op_str:
            reg = ins.op_str.split(',')[0]
            regs[reg] = ins.address + ins.size + struct.unpack_from('<i', b, ins.address + ins.size - 4)[0]
        elif ins.mnemonic == 'mov' and re.fullmatch(r'qword ptr \[r\w+\], (r\w+)', ins.op_str):
            t = regs.get(ins.op_str.split(', ')[1])
            if t and VT_LO <= t < VT_HI and base + TEXT_LO <= q(t) < base + TEXT_HI:
                own = t
        elif ins.mnemonic == 'ret' or (ins.mnemonic == 'jmp' and '[' not in ins.op_str):
            break
    if own is not None:
        out.setdefault('%x' % own, set()).add(nm)
out = {k: '|'.join(sorted(v)) for k, v in out.items()}
json.dump(out, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vtnames.json'), 'w'), indent=0)
print(len(out), 'vtables named')
