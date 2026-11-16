# settings.py [off=value ...] : print (and optionally write, host-side) live SpawnSettings fields.
# Offsets hex; values ending in 'f' or containing '.' are floats, else uint32.
import struct, sys
from live import Live

L = Live()
w = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
mgr = L.q(L.q(w + 0x13c0) + 0x38)
s = L.q(mgr + 0x10)
assert L.q(s) == L.base + 0x363cc80, 'unexpected SpawnSettings vtable'
mem = open('/proc/%d/mem' % L.pid, 'r+b', buffering=0)
for kv in sys.argv[1:]:
    o, v = kv.split('=')
    o = int(o, 16)
    mem.seek(s + o)
    mem.write(struct.pack('<f', float(v.rstrip('f'))) if ('.' in v or v.endswith('f')) else struct.pack('<I', int(v)))
b = L.read(s + 0x38, 0x70)
u = struct.unpack('<28I', b)
fl = struct.unpack('<28f', b)
print('SpawnSettings %x: maxSpawned=%d mult[3c..60]=%s perframe=%d/%d +70=%g +74=%g +78=%g +7c=%g' % (
    s, u[0], ['%.3g' % x for x in fl[1:11]], u[12], u[13], fl[14], fl[15], fl[16], fl[17]))
