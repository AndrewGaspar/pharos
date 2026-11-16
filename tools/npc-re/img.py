# Shared helpers for the decrypted image dump (file offset = RVA). IMG env var overrides the path.
import mmap, os, struct

IMG = os.environ.get('IMG', '/tmp/claude-1000/-home-ajg-code-ac-dlss/249d0810-7bc6-42cc-9509-5599a9b80e14/scratchpad/npc/img')
_fh = open(IMG, 'rb')
m = mmap.mmap(_fh.fileno(), 0, access=mmap.ACCESS_READ)
# Wine writes the actual load base into OptionalHeader.ImageBase when it relocates.
base = struct.unpack_from('<Q', m, struct.unpack_from('<I', m, 0x3c)[0] + 48)[0]


def q(r): return struct.unpack_from('<Q', m, r)[0]
def d(r): return struct.unpack_from('<I', m, r)[0]
def f(r): return struct.unpack_from('<f', m, r)[0]


def cstr(ptr, absolute=True):
    r = ptr - base if absolute else ptr
    if not 0 <= r < len(m): return None
    e = m.find(b'\0', r, r + 120)
    t = m[r:e]
    return t.decode() if t and all(32 <= c < 127 for c in t) else None


def is_img(v): return base <= v < base + len(m)
