# children.py <parent-typeinfo-rva-as-stored> : list TypeInfo records whose +0x28 parent == base+rva,
# with name, size, ctor rva and the vtable rva the ctor installs (first lea before 'mov %rax,(%rbx)')
import mmap, struct, re, subprocess, sys
import numpy as np
IMG = '/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/lod/envU-1.image'
f = open(IMG, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
base = 0x6fffed060000
q = np.frombuffer(m, dtype='<u8', count=len(m) // 8)
target = base + int(sys.argv[1], 16)
def s_at(ptr):
    r = ptr - base
    if not 0 <= r < len(m): return None
    e = m.find(b'\0', r, r + 100)
    try: s = m[r:e].decode()
    except Exception: return None
    return s if s.isprintable() and len(s) > 2 else None
for i in np.nonzero(q == target)[0]:
    rec = int(i) * 8 - 0x28
    name = s_at(struct.unpack_from('<Q', m, rec)[0])
    if not name: continue
    size = struct.unpack_from('<Q', m, rec + 0x10)[0]
    ctor = struct.unpack_from('<Q', m, rec + 0x40)[0] - base
    vt = None
    if 0 < ctor < len(m):
        out = subprocess.run(['x86_64-w64-mingw32-objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64',
                              f'--start-address={ctor}', f'--stop-address={ctor + 0x40}', IMG],
                             capture_output=True, text=True).stdout
        mm = re.findall(r'lea\s+0x[0-9a-f]+\(%rip\),%r[a-z0-9]+\s+# 0x([0-9a-f]+)', out)
        vt = mm[0] if mm else None
    print(f'{name:40s} size {size:#6x} ctor {ctor:#x} vt {vt}')
