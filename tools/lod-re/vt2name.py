# vt2name.py <vtable_rva>... : class name for a vtable (ctor lea xref -> TypeInfo whose +0x40 ctor covers it)
import mmap,struct,re,sys,numpy as np
IMG='/home/ajg/Games/aco-dlss-instances/t1/out/ACOrigins.image'
_f=open(IMG,'rb'); m=mmap.mmap(_f.fileno(),0,access=mmap.ACCESS_READ)
base=0x6fffed070000
views=[np.frombuffer(m,dtype='<i4',count=(len(m)-k)//4,offset=k) for k in range(4)]
q64=np.frombuffer(m,dtype='<u8',count=len(m)//8)
def name_at(ptr):
    r=ptr-base
    if not (0<=r<len(m)): return None
    e=m.find(b'\0',r,r+100)
    try: s=m[r:e].decode()
    except: return None
    return s if s.isprintable() and len(s)>2 else None
for a in sys.argv[1:]:
    t=int(a,16); refs=[]
    for k in range(4):
        v=views[k]; pos=np.arange(len(v),dtype=np.int64)*4+k
        refs+=[int(pos[i]) for i in np.nonzero(v.astype(np.int64)==(t-(pos+4)))[0]]
    names=set()
    for r in refs:
        # candidate ctor starts: TypeInfo +0x40 pointing to base+[r-0x60, r]
        lo,hi=base+r-0x60, base+r
        idx=np.nonzero((q64>=lo)&(q64<=hi))[0]
        for i in idx:
            ti=int(i)*8-0x40
            if ti<0: continue
            n=name_at(struct.unpack_from('<Q',m,ti)[0])
            if n: names.add((n, hex(struct.unpack_from('<Q',m,ti+0x10)[0])))
    print(a, sorted(names) or [hex(x) for x in refs[:4]])
