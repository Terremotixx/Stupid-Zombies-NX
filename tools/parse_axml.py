#!/usr/bin/env python3
import struct,sys
D=open(sys.argv[1],'rb').read()
u16=lambda o:struct.unpack_from('<H',D,o)[0]
u32=lambda o:struct.unpack_from('<I',D,o)[0]
# chunks: type u16, headerSize u16, size u32
# string pool first
strings=[]
resource_map=[]
o=8
while o+8<=len(D):
    typ,hs,sz=u16(o),u16(o+2),u32(o+4)
    if sz<8: break
    if typ==0x0001: # string pool
        count=u32(o+8); styles=u32(o+12); flags=u32(o+16); str_start=u32(o+20)
        utf8=bool(flags & 0x100)
        offs=[u32(o+hs+i*4) for i in range(count)]
        base=o+str_start
        def dec_len8(p):
            x=D[p]; p+=1
            if x&0x80: x=((x&0x7f)<<8)|D[p]; p+=1
            return x,p
        def dec_len16(p):
            x=u16(p); p+=2
            if x&0x8000: x=((x&0x7fff)<<16)|u16(p); p+=2
            return x,p
        for off in offs:
            p=base+off
            if utf8:
                _,p=dec_len8(p); bl,p=dec_len8(p); s=D[p:p+bl].decode('utf-8','replace')
            else:
                ln,p=dec_len16(p); s=D[p:p+ln*2].decode('utf-16le','replace')
            strings.append(s)
    elif typ==0x0180: # resource map
        resource_map=[u32(i) for i in range(o+hs,o+sz,4)]
    elif typ==0x0102: # start element
        # node header 16 then ext at +16: ns,name,attrStart,attrSize,attrCount,id,class,style
        name_idx=u32(o+20)
        name=strings[name_idx] if name_idx<len(strings) else f'#{name_idx}'
        attr_start=u16(o+24); attr_size=u16(o+26); attr_count=u16(o+28)
        p=o+16+attr_start
        attrs=[]
        for i in range(attr_count):
            a=p+i*attr_size
            ns,namei,raw=u32(a),u32(a+4),u32(a+8)
            vsize=u16(a+12); vtype=D[a+15]; data=u32(a+16)
            an=strings[namei] if namei<len(strings) else f'#{namei}'
            rawv=None if raw==0xffffffff else strings[raw]
            if rawv is not None: val=rawv
            elif vtype==0x03 and data<len(strings): val=strings[data]
            elif vtype==0x12: val=bool(data)
            else: val=data
            attrs.append((an,val,vtype,data))
        if name in ('manifest','uses-sdk','application','activity','activity-alias'):
            print(f'<{name}>')
            for an,val,vt,data in attrs:
                if an in ('package','versionName','versionCode','name','screenOrientation','configChanges','hardwareAccelerated','minSdkVersion','targetSdkVersion','resizeableActivity','exported','theme'):
                    print(f'  {an}={val!r} (type=0x{vt:02x} data=0x{data:x})')
    o+=sz
