#!/usr/bin/env python3
"""extract_entrypoints.py -- recover libunity.so's UnityPlayer JNI natives.

The loader calls these by RVA (unity_mod.load_virtbase + offset), so the
offsets in source/unity_entrypoints.h are valid for ONE EXACT build of
libunity.so. Point this at your own copy after any game update:

    python3 tools/extract_entrypoints.py libunity.so

HOW IT WORKS
  Unity registers these natives with RegisterNatives from JNINativeMethod[]
  tables -- {const char *name, const char *sig, void *fnPtr} triples. In a PIC
  shared object every one of those three fields is filled in at load time by an
  R_AARCH64_RELATIVE relocation, so the tables can be read straight out of
  .rela.dyn without disassembling or running anything.

  Note the tables live in .data for this build, NOT .data.rel.ro -- scan both.

  A run of consecutive 24-byte triples whose 1st field is an identifier-shaped
  string and whose 2nd is a JNI signature "(...)X" is a JNINativeMethod[]. The
  UnityPlayer drive surface is the 26-entry table; the others are ARCore /
  Camera2 / HFP / audio-volume / FMOD / proxy helpers this port does not call.
"""
import sys, struct

def sections(d):
    shoff=struct.unpack('<Q',d[40:48])[0]; shent=struct.unpack('<H',d[58:60])[0]
    shnum=struct.unpack('<H',d[60:62])[0]; shstrndx=struct.unpack('<H',d[62:64])[0]
    def sh(i):
        o=shoff+i*shent; return struct.unpack('<IIQQQQIIQQ',d[o:o+64])
    stro=sh(shstrndx)[4]; out={}
    for i in range(shnum):
        n,t,f,a,off,sz,l,inf,al,es=sh(i)
        nm=d[stro+n:d.index(b'\0',stro+n)].decode()
        out[nm]=dict(type=t,addr=a,off=off,size=sz)
    return out

def main(path):
    d=open(path,'rb').read()
    sec=sections(d)
    def va2off(va):
        for nm,s in sec.items():
            if s['addr'] and s['addr']<=va<s['addr']+s['size'] and s['type']!=8:
                return s['off']+(va-s['addr'])
        return None
    rel={}
    for name in ('.rela.dyn','.rela.plt'):
        s=sec.get(name)
        if not s: continue
        for i in range(s['size']//24):
            o=s['off']+i*24; off,info,add=struct.unpack('<QQq',d[o:o+24])
            if (info&0xffffffff)==1027: rel[off]=add          # R_AARCH64_RELATIVE
    def cstr(va,maxlen=300):
        o=va2off(va)
        if o is None: return None
        end=d.find(b'\0',o,o+maxlen)
        if end<0: return None
        try: return d[o:end].decode('ascii')
        except Exception: return None

    entries=[]
    for name in ('.data','.data.rel.ro'):
        s=sec.get(name)
        if not s: continue
        va=s['addr']; hi=s['addr']+s['size']
        while va+24<=hi:
            n,g,f=rel.get(va),rel.get(va+8),rel.get(va+16)
            hit=False
            if None not in (n,g,f):
                nm,sg=cstr(n),cstr(g)
                if nm and sg and sg.startswith('(') and ')' in sg and nm.replace('_','a').isalnum():
                    entries.append((va,nm,sg,f)); va+=24; hit=True
            if not hit: va+=8

    runs=[];cur=[];prev=None
    for r in entries:
        if prev is not None and r[0]!=prev: runs.append(cur); cur=[]
        cur.append(r); prev=r[0]+24
    if cur: runs.append(cur)

    # JNI_OnLoad is an exported symbol -- read it from .dynsym
    onload=None
    ds,st=sec.get('.dynsym'),sec.get('.dynstr')
    if ds and st:
        for i in range(ds['size']//24):
            o=ds['off']+i*24
            nameoff,info,other,shndx,value,size=struct.unpack('<IBBHQQ',d[o:o+24])
            nm=d[st['off']+nameoff:d.index(b'\0',st['off']+nameoff)].decode('utf-8','replace')
            if nm=='JNI_OnLoad': onload=value

    print("JNI_OnLoad (exported): 0x%x" % onload if onload else "JNI_OnLoad: NOT FOUND")
    for r in runs:
        tag = "  <-- UnityPlayer drive surface" if len(r)>=20 else ""
        print("\n## table @0x%x  (%d methods)%s"%(r[0][0],len(r),tag))
        for va,nm,sg,f in r:
            print("   %-38s %-46s 0x%x"%(nm,sg,f))
    big=[r for r in runs if len(r)>=20]
    if big:
        print("\n\n/* ---- paste into source/unity_entrypoints.h ---- */")
        if onload: print("#define OFF_JNI_OnLoad%-36s0x%x" % ("", onload))
        for va,nm,sg,f in big[0]:
            macro = "OFF_" + nm
            print("#define %-46s 0x%x" % (macro, f))

if __name__=='__main__':
    main(sys.argv[1] if len(sys.argv)>1 else 'libunity.so')
