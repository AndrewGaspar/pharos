# xref.py <name>... : find RIP-relative references (disp32 at insn end) to NUL-terminated strings.
import mmap, re, sys, numpy as np
from img import IMG
f=open(IMG,'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
a=np.frombuffer(m,dtype=np.uint8)
targets={}
for n in sys.argv[1:]:
    b=n.encode()
    for mt in re.finditer(b'\0'+re.escape(b)+b'\0', m):
        targets[mt.start()+1]=n
N=len(a)-4
views=[np.frombuffer(m,dtype='<i4',count=(len(m)-k)//4,offset=k) for k in range(4)]
for t,n in targets.items():
    hits=[]
    for k in range(4):
        v=views[k]
        pos=np.arange(len(v),dtype=np.int64)*4+k
        idx=np.nonzero(v.astype(np.int64)==(t-(pos+4)))[0]
        hits+= [int(pos[i]) for i in idx]
    print(n, hex(t), [hex(h) for h in sorted(hits)[:10]])
