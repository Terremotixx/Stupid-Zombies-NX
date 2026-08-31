import struct,sys
from capstone import *
path,start,end = sys.argv[1], int(sys.argv[2],16), int(sys.argv[3],16)
d=open(path,'rb').read()
e_shoff=struct.unpack('<Q',d[40:48])[0]; shent=struct.unpack('<H',d[58:60])[0]
shnum=struct.unpack('<H',d[60:62])[0]; shstrndx=struct.unpack('<H',d[62:64])[0]
def sh(i):
    o=e_shoff+i*shent; return struct.unpack('<IIQQQQIIQQ',d[o:o+64])
stro=sh(shstrndx)[4]
for i in range(shnum):
    n,t,f,a,off,sz,l,inf,al,es=sh(i)
    if d[stro+n:d.index(b'\0',stro+n)]==b'.text': va0,fo0,tsz=a,off,sz
md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN); md.detail=False
fo=fo0+(start-va0)
code=d[fo:fo+(end-start)]
for ins in md.disasm(code,start):
    w=struct.unpack_from('<I',d,fo0+(ins.address-va0))[0]
    print("0x%08x  %08x  %-8s %s"%(ins.address,w,ins.mnemonic,ins.op_str))
