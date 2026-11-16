import mmap,struct,sys
f=open('/home/ajg/Games/aco-dlss-instances/t1/out/ACOrigins.image','rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
base=0x6fffed070000
def s_at(rva):
    if not (0<=rva<len(m)-1): return None
    e=m.find(b'\0',rva,rva+80)
    if e<0: return None
    try: t=m[rva:e].decode()
    except: return None
    return t if len(t)>=3 and t.isprintable() else None
def dump(rva,n):
    for i in range(n):
        q=struct.unpack_from('<Q',m,rva+8*i)[0]
        lo,hi=struct.unpack_from('<II',m,rva+8*i); fl=struct.unpack_from('<ff',m,rva+8*i)
        lab=''
        if base<=q<base+len(m): lab=s_at(q-base) or ('->rva %x'%(q-base))
        print(' %x: %016x  %-30s %s'%(rva+8*i,q,lab,'(%g, %g)'%fl if all(abs(x)<1e7 and (x==0 or abs(x)>1e-6) for x in fl) else ''))
for a in sys.argv[1:]:
    r,n=(a.split(':')+['16'])[:2]; print('---',r); dump(int(r,16),int(n))
