# callers.py <target-rva> : E8 rel32 call sites of target, with the edx immediate set just before (if any)
import mmap, re, struct, subprocess, sys
import numpy as np
IMG = '/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/lod/envU-1.image'
f = open(IMG, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
t = int(sys.argv[1], 16)
CODE_END = 0x3538e00
a = np.frombuffer(m, dtype=np.uint8, count=CODE_END)
e8 = np.nonzero(a[:-5] == 0xE8)[0]
hits = []
for p in e8:
    p = int(p)
    if p + 5 + struct.unpack_from('<i', m, p + 1)[0] == t: hits.append(p)
names = {0: 'AdaptiveQuality', 1: 'Antialiasing', 2: 'Bloom', 3: 'Character', 4: 'CharacterTexture', 5: 'Cloud',
         6: 'Clutter', 7: 'CrowdDensity', 8: 'Culling', 9: 'DOF', 10: 'Environment', 11: 'Fog', 12: 'Lighting',
         13: 'Particle', 14: 'PostEffect', 15: 'Reflection', 16: 'Shadow', 17: 'SSAO', 18: 'Terrain',
         19: 'Tessellation', 20: 'Texture', 21: 'TextureFiltering', 22: 'Water', 23: 'Wind'}
for p in hits:
    out = subprocess.run(['x86_64-w64-mingw32-objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64',
                          f'--start-address={p - 24}', f'--stop-address={p + 5}', IMG], capture_output=True, text=True).stdout
    imm = re.findall(r'mov\s+\$0x([0-9a-f]+),%edx', out)
    idx = int(imm[-1], 16) if imm else None
    print('%08x idx=%s %s' % (p, idx, names.get(idx, '?')))
