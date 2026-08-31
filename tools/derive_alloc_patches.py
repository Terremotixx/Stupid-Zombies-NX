import struct,sys
from capstone import *
L='/Users/arnavkamath/Documents/Claude/abreloaded/lib/arm64-v8a/libunity.so'
d=open(L,'rb').read()
e_shoff=struct.unpack('<Q',d[40:48])[0]; shent=struct.unpack('<H',d[58:60])[0]
shnum=struct.unpack('<H',d[60:62])[0]; shstrndx=struct.unpack('<H',d[62:64])[0]
def sh(i):
    o=e_shoff+i*shent; return struct.unpack('<IIQQQQIIQQ',d[o:o+64])
stro=sh(shstrndx)[4]
for i in range(shnum):
    n,t,f,a,off,sz,l,inf,al,es=sh(i)
    if d[stro+n:d.index(b'\0',stro+n)]==b'.text': va0,fo0=a,off

def bm(N,immr,imms,sf):
    length=(N<<6)|(~imms&0x3f); ln=length.bit_length()-1
    if ln<1: return None
    size=1<<ln
    if size>(64 if sf else 32): return None
    R=immr&(size-1); S=imms&(size-1)
    if S==size-1: return None
    we=(1<<(S+1))-1; we=((we>>R)|(we<<(size-R)))&((1<<size)-1)
    n=64 if sf else 32; out=0
    for i in range(0,n,size): out|=we<<i
    return out&((1<<n)-1)

md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
START,END=0x718000,0x722000
hits=[]
for va in range(START,END,4):
    w=struct.unpack_from('<I',d,fo0+(va-va0))[0]
    kind=None;new=None;why=None
    # MOVZ/MOVK imm16=0x1000 hw=1
    if (w&0x7F800000) in (0x52800000,0x72800000) and ((w>>21)&3)==1 and ((w>>5)&0xFFFF)==0x1000:
        new=(w & ~(0xFFFF<<5)) | (0x0400<<5); kind='MOV*'; why='imm16 0x1000->0x0400 (256MB->64MB)'
    # MOVN imm16=0xF000 hw=1  =>  ~(0xF000<<16) == 0x0FFFFFFF == 256MB-1
    elif (w&0x7F800000)==0x12800000 and ((w>>21)&3)==1 and ((w>>5)&0xFFFF)==0xF000:
        new=(w & ~(0xFFFF<<5)) | (0xFC00<<5); kind='MOVN'; why='0x0fffffff -> 0x03ffffff (256MB-1 -> 64MB-1)'
    # UBFM immr=28, imms>=28
    elif ((w&0x7F800000)==0x53000000 or (w&0xFFC00000)==0xD3400000):
        sf=(w>>31)&1;N=(w>>22)&1;immr=(w>>16)&0x3f;imms=(w>>10)&0x3f
        if immr==28 and imms>=28 and ((sf==1 and N==1) or (sf==0 and N==0)):
            if imms==(63 if sf else 31):
                ni,ns=26,imms; why='lsr #28->#26'
            else:
                ni,ns=26,imms-2; why='ubfx lsr#28->#26, width %d preserved'%(imms-immr+1)
            new=(w & ~((0x3f<<16)|(0x3f<<10))) | (ni<<16) | (ns<<10); kind='UBFM'
    # AND/ORR/EOR immediate clearing low 28 bits
    elif (w&0x1F800000)==0x12000000:
        sf=(w>>31)&1;N=(w>>22)&1;immr=(w>>16)&0x3f;imms=(w>>10)&0x3f
        v=bm(N,immr,imms,sf)
        if v is not None and sf==1 and v!=0 and (v & 0x1FFFFFFF)==(1<<28):
            ni,ns=immr+2,imms+2
            nv=bm(N,ni,ns,sf)
            if nv is not None and (nv & 0x3FFFFFF)==0:
                new=(w & ~((0x3f<<16)|(0x3f<<10))) | (ni<<16) | (ns<<10)
                kind='AND-imm'; why='mask 0x%x -> 0x%x'%(v,nv)
    # ADD/SUB shifted lsl #28
    elif (w&0x7F200000) in (0x0B000000,0x4B000000,0x8B000000,0xCB000000):
        if ((w>>22)&3)==0 and ((w>>10)&0x3f)==28:
            new=(w & ~(0x3f<<10)) | (26<<10); kind='ADD/SUB'; why='lsl #28 -> #26'
    # MOV wN, #0x0fffffff  (256MB-1 round-up constant)  -> ORR immediate w/ WZR
    elif (w&0x7F800000)==0x12000000 or True:
        pass
    if new is not None and new!=w:
        hits.append((va,w,new,kind,why))

# separate pass: 32/64-bit immediate materialisation of 0x0FFFFFFF
for va in range(START,END,4):
    w=struct.unpack_from('<I',d,fo0+(va-va0))[0]
    if (w&0x1F800000)==0x12000000:
        sf=(w>>31)&1;N=(w>>22)&1;immr=(w>>16)&0x3f;imms=(w>>10)&0x3f
        if ((w>>5)&31)==31:      # ORR/MOV from WZR/XZR => materialised constant
            v=bm(N,immr,imms,sf)
            if v==0x0FFFFFFF:
                # 64MB-1 == 0x03FFFFFF : N=0, immr=0, imms=25 (32-bit) ; keep sf
                nw=(w & ~((1<<22)|(0x3f<<16)|(0x3f<<10))) | (0<<22)|(0<<16)|(25<<10)
                assert bm(0,0,25,sf)==0x03FFFFFF, hex(bm(0,0,25,sf))
                hits.append((va,w,nw,'MOV-imm','0x0fffffff -> 0x03ffffff (256MB-1 -> 64MB-1)'))

hits.sort()
for va,w,nw,k,why in hits:
    ins=next(md.disasm(struct.pack('<I',w),va))
    print("  { 0x%06x, 0x%08x, 0x%08x },  /* %-10s %-30s %s */"%(va,w,nw,ins.mnemonic+' '+ins.op_str,why,k))
print("\nTOTAL %d sites"%len(hits))
