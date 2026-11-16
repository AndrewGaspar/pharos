# spawncount.py [samples] [interval s] : non-intrusive spawn-budget readout (host /proc read).
# SpawningManager (vtable rva 363beb0): +0x10 SpawnSettings*, +0x58 -> pool list {ptr, count<<17},
# entries 0x28 bytes with the live count at +0x10 (what the game compares against the cap).
import sys, time
from live import Live

L = Live()
def find_mgr():
    # Same chain the game uses: [rva 0x4c27f18] -> +0xc0 World -> +0x13c0 -> +0x38 SpawningManager.
    w = L.q(L.q(L.base + 0x4c27f18) + 0xc0)
    m = L.q(L.q(w + 0x13c0) + 0x38)
    assert L.q(m) == L.base + 0x363beb0, 'unexpected SpawningManager vtable'
    return m
mgr = find_mgr()
n = int(sys.argv[1]) if len(sys.argv) > 1 else 1
dt = float(sys.argv[2]) if len(sys.argv) > 2 else 5
for i in range(n):
    settings = L.q(mgr + 0x10)
    p = L.q(mgr + 0x58)
    arr, cnt = L.q(p), L.d(p + 8) >> 17
    total = sum(L.d(arr + 0x28 * k + 0x10) for k in range(cnt))
    print('%s spawned=%d cap=%d' % (time.strftime('%H:%M:%S'), total, L.d(settings + 0x38)), flush=True)
    if i + 1 < n: time.sleep(dt)
