# Host-side access to the running game's memory through /proc/<pid>/mem (read-only).
import os, re, struct, subprocess
import numpy as np


def game_pid():
    inst = os.environ['ACO_INST']  # t1 / t3: the instance whose game to read
    here = os.path.dirname(os.path.abspath(__file__))
    return int(subprocess.run([os.path.join(here, 'gamepid.sh'), inst], capture_output=True, text=True, check=True).stdout)


class Live:
    def __init__(self, pid=None):
        self.pid = pid or game_pid()
        self.mem = open('/proc/%d/mem' % self.pid, 'rb', buffering=0)
        self.base = None
        self.regions = []  # (start, end, perms, path)
        for line in open('/proc/%d/maps' % self.pid):
            m = re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)', line)
            s, e, perms, path = int(m[1], 16), int(m[2], 16), m[3], m[4]
            if self.base is None and path.endswith('ACOrigins.exe'): self.base = s
            self.regions.append((s, e, perms, path))

    def read(self, addr, n):
        try:
            self.mem.seek(addr)
            return self.mem.read(n)
        except OSError:
            return None

    def q(self, a): b = self.read(a, 8); return struct.unpack('<Q', b)[0] if b and len(b) == 8 else None
    def d(self, a): b = self.read(a, 4); return struct.unpack('<I', b)[0] if b and len(b) == 4 else None
    def f(self, a): b = self.read(a, 4); return struct.unpack('<f', b)[0] if b and len(b) == 4 else None

    def heap_regions(self, maxsize=1 << 31):
        # Writable private mappings (Windows heap / VirtualAlloc). Skip huge GPU/shared mappings.
        for s, e, perms, path in self.regions:
            if perms.startswith('rw') and perms[3] == 'p' and (path == '' or path.startswith('[heap') or path.endswith('ACOrigins.exe')) and e - s <= maxsize:
                yield s, e

    def scan_q(self, values, step=8, maxhits=1 << 20):
        """Addresses whose aligned qword is in `values` (iterable of ints). Returns {value: [addr]}."""
        vals = np.array(sorted(set(values)), dtype=np.uint64)
        out = {int(v): [] for v in vals}
        for s, e in self.heap_regions():
            for cs in range(s, e, 64 << 20):
                ce = min(e, cs + (64 << 20))
                b = self.read(cs, ce - cs)
                if not b: continue
                a = np.frombuffer(b[:len(b) // 8 * 8], dtype='<u8')
                idx = np.nonzero(np.isin(a, vals))[0]
                for i in idx:
                    out[int(a[i])].append(cs + int(i) * 8)
        return out

    def vt(self, rva):
        v = self.base + rva
        return self.scan_q([v])[v]
