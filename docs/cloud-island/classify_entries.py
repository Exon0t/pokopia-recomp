# classify_entries.py: decide which computed-address targets in main's online band are functions and which are in-function labels.
import struct,lz4.block,re,bisect,sys
C=r"C:/Users/Minek/Documents/Projects/pokopia-tests/cloud-island/"
d=open(r"C:/Users/Minek/Downloads/pokopia-recomp/Pokopia_v2.0.1a/Pokopia_v2.0.1a/updates/655360/exefs/main","rb").read()
foff,moff,size=struct.unpack_from('<III',d,0x10); csz=struct.unpack_from('<I',d,0x60)[0]
text=lz4.block.decompress(d[foff:foff+csz],uncompressed_size=size); W=struct.unpack('<%dI'%(len(text)//4),text)
def sx(v,b): return v-(1<<b) if v&(1<<(b-1)) else v
sites={}; pa={}; bl=set(); br=set()
for i,w in enumerate(W):
    off=i*4
    if (w&0x9F000000)==0x10000000: sites.setdefault(off+sx((((w>>5)&0x7ffff)<<2)|((w>>29)&3),21),[]).append(off)
    elif (w&0x9F000000)==0x90000000: pa[w&31]=((off&~0xfff)+(sx((((w>>5)&0x7ffff)<<2)|((w>>29)&3),21)<<12),off)
    elif (w&0xFFC00000)==0x91000000:
        rn=(w>>5)&31
        if rn in pa and off-pa[rn][1]<=16: sites.setdefault(pa[rn][0]+((w>>10)&0xfff),[]).append(off)
    if (w&0xFC000000)==0x94000000: bl.add(off+sx(w&0x3ffffff,26)*4)
    elif (w&0xFC000000)==0x14000000: br.add(off+sx(w&0x3ffffff,26)*4)                 # b
    elif (w&0xFF000010)==0x54000000 or (w&0x7E000000)==0x34000000: br.add(off+sx((w>>5)&0x7ffff,19)*4)   # b.cond, cbz/cbnz
    elif (w&0x7E000000)==0x36000000: br.add(off+sx((w>>5)&0x3fff,14)*4)               # tbz/tbnz
def term(x): return x in (0xD65F03C0,0xD65F0BFF,0xD65F0FFF) or (x&0xFC000000)==0x14000000 or (x&0xFFFF0000)==0 or (x&0xFFFFFC1F)==0xD61F0000
rd=lambda f:[int(x,16) for x in open(C+f).read().split()]
base=set(rd("main-missing-entries-0.11.1.txt"))|{x for x in rd("main-missing-entries-2.txt") if 0x1730000<=x<0x1770000}
comp=sorted(set(rd("computed-candidates.txt")))
def is_label(t):
    # a label: something inside the code branches to it directly, or it does not follow a terminator
    return t in br or not term(W[t//4-1])
label=[t for t in comp if is_label(t)]; func=[t for t in comp if t not in label]
if __name__=='__main__':
    print(len(comp),'computed:',len(func),'functions,',len(label),'labels')
    for t in (0x176302c,0x173a1d4,0x173b03c,0x1734e1c,0x1763170,0x173a200):
        print(hex(t),'in list' if t in comp else 'NOT in list','->','label' if is_label(t) else 'function','| direct-branch target' if t in br else '','| after terminator' if term(W[t//4-1]) else '| falls through')
    final=sorted(base|set(func))
    p=r"C:/Users/Minek/Downloads/pokopia-recomp/_cloud-build/PokopiaRecomp-0.11.1/tools/build/recomp/games/pkp/config.py"; s=open(p,encoding='utf-8').read()
    mm=re.search(r"'main': \[([^\]]*)\]", s); s=s[:mm.start()]+"'main': ["+', '.join(hex(x) for x in final)+"]"+s[mm.end():]
    if '--write' in sys.argv:
        open(p,'w',encoding='utf-8',newline='\n').write(s); open(C+"main-extra-entries-final.txt","w").write("\n".join(hex(x) for x in final))
    print('entries',len(final),'(written)' if '--write' in sys.argv else '(dry run)')
