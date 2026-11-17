# callargs.py <target rva> [window=0x30] : every direct call to <target>, with the instructions just
# before it (to see immediate arguments: mov $imm,%edx / %r8d ...). Uses rel32.py's scan + objdump.
import subprocess, sys, os
here = os.path.dirname(os.path.abspath(__file__))
from img import IMG

t = sys.argv[1]
win = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x30
out = subprocess.run([sys.executable, os.path.join(here, 'rel32.py'), t], capture_output=True, text=True).stdout
sites = [int(s.split(':')[1], 16) for s in out.split(':', 1)[1].split() if s.startswith(('call:', 'jmp:'))]
for s in sites:
    d = subprocess.run(['x86_64-w64-mingw32-objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64',
                        '--start-address=%d' % (s - win), '--stop-address=%d' % (s + 5), IMG],
                       capture_output=True, text=True).stdout.splitlines()[7:]
    lines = [l.split('\t')[-1].strip() for l in d if '\t' in l]
    print('%x: %s' % (s, ' ; '.join(lines[-8:])))
