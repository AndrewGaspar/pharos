# typeinfo.py <ClassName>... : find Anvil TypeInfo records (name ptr at +0, ctor at +0x40) and the vtable the ctor installs
import mmap,struct,re,sys,subprocess
IMG='/home/ajg/Games/aco-dlss-instances/t1/out/ACOrigins.image'
_f=open(IMG,'rb'); m=mmap.mmap(_f.fileno(),0,access=mmap.ACCESS_READ)
base=0x6fffed070000
def q(r): return struct.unpack_from('<Q',m,r)[0]
for name in sys.argv[1:]:
    b=name.encode()
    for mt in re.finditer(re.escape(b)+b'\0', m):
        s=mt.start()
        if s>0 and m[s-1:s].isalnum(): continue  # must be a whole string
        refs=[x.start() for x in re.finditer(re.escape(struct.pack('<Q',base+s)),m)]
        for r in refs:
            size=q(r+0x10); ctor=q(r+0x40); parent=q(r+0x28)
            pn=''
            if base<=parent<base+len(m):
                pq=q(parent-base); 
                if base<=pq<base+len(m): pn=m[pq-base:m.find(b'\0',pq-base)].decode(errors='replace')
            line=f'{name}: str {s:#x} typeinfo {r:#x} size {size:#x} parent {pn!r}'
            if base<=ctor<base+len(m):
                c=ctor-base
                out=subprocess.run(['x86_64-w64-mingw32-objdump','-D','-b','binary','-m','i386:x86-64',f'--start-address={c}',f'--stop-address={c+0x120}',IMG],capture_output=True,text=True).stdout
                vt=re.findall(r'lea\s+0x[0-9a-f]+\(%rip\),%r[a-z0-9]+\s+# (0x[0-9a-f]+)',out)
                line+=f' ctor {c:#x} leas {vt[:3]}'
            print(line)
