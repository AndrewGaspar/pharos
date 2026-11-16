# settab.py : print the 24-entry graphics settings descriptor table (RVA 0x49af000, 0x2c bytes each)
from img import *

T = 0x49af000
for i in range(24):
    r = T + i * 0x2c
    words = [d(r + o) for o in range(0, 0x2c, 4)]
    name = None
    for o in range(0, 0x2c, 8):
        v = q(r + o) if (r + o) % 8 == 0 else 0
        if is_img(v) and cstr(v): name = cstr(v)
    print(i, name, ' '.join('%x' % w for w in words))
