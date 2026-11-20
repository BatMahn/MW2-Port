#!/usr/bin/env python3
"""le2elf - the DOS MW2.EXE (LE, DOS4GW) as a 32-bit ELF image for mw2emu.py: le2elf.py MW2.EXE mw2dos.elf"""
import struct,sys,re
def load_le(path):
    d=open(path,'rb').read()
    off=None
    o3c=struct.unpack_from('<I',d,0x3c)[0]
    if d[o3c:o3c+2]==b'LE': off=o3c
    else:
        for m in re.finditer(b'LE\x00\x00\x00\x00\x00\x00',d): off=m.start(); break
    H=off
    psz=struct.unpack_from('<I',d,H+0x28)[0]; last=struct.unpack_from('<I',d,H+0x2c)[0]
    npages=struct.unpack_from('<I',d,H+0x14)[0]
    objtab=struct.unpack_from('<I',d,H+0x40)[0]+H; nobj=struct.unpack_from('<I',d,H+0x44)[0]
    fpt=struct.unpack_from('<I',d,H+0x68)[0]+H; frt=struct.unpack_from('<I',d,H+0x6c)[0]+H
    dp=struct.unpack_from('<I',d,H+0x80)[0]
    # data pages offset: for bound DOS4GW executables the offset is relative to file start? try both
    objs=[]
    for i in range(nobj):
        vs,base,fl,pidx,pcnt,_=struct.unpack_from('<6I',d,objtab+24*i); objs.append([vs,base,fl,pidx,pcnt])
    mem={}
    for oi,(vs,base,fl,pidx,pcnt) in enumerate(objs):
        buf=bytearray((vs+psz-1)//psz*psz or psz)
        for p in range(pcnt):
            pg=pidx-1+p; o=dp+pg*psz; n=psz if pg!=npages-1 else last
            buf[p*psz:p*psz+n]=d[o:o+n]
        mem[oi]=buf
    nfix=0
    for oi,(vs,base,fl,pidx,pcnt) in enumerate(objs):
        for p in range(pcnt):
            pg=pidx-1+p
            a=struct.unpack_from('<I',d,fpt+4*pg)[0]; b=struct.unpack_from('<I',d,fpt+4*pg+4)[0]
            q=frt+a
            while q<frt+b:
                src=d[q]; flg=d[q+1]; q+=2
                if src&0x20: cnt=d[q]; q+=1; offs=None
                else: so=struct.unpack_from('<h',d,q)[0]; q+=2
                if flg&3!=0: raise Exception('non-internal fixup %x'%flg)
                if flg&0x40: tobj=struct.unpack_from('<H',d,q)[0]; q+=2
                else: tobj=d[q]; q+=1
                st=src&0xf; toff=0
                if st!=2:
                    if flg&0x10: toff=struct.unpack_from('<I',d,q)[0]; q+=4
                    else: toff=struct.unpack_from('<H',d,q)[0]; q+=2
                if src&0x20:
                    lst=[struct.unpack_from('<h',d,q+2*k)[0] for k in range(cnt)]; q+=2*cnt
                else: lst=[so]
                tgt=objs[tobj-1][1]+toff
                for so in lst:
                    pos=p*psz+so
                    if pos<0 or pos+4>len(mem[oi]): continue
                    if st==7: struct.pack_into('<I',mem[oi],pos,tgt&0xffffffff); nfix+=1
                    elif st==8:  # self-relative 32
                        here=base+pos+4; struct.pack_into('<i',mem[oi],pos,(tgt-here)); nfix+=1
                    elif st==6: struct.pack_into('<I',mem[oi],pos,tgt&0xffffffff); nfix+=1
                    elif st==5: struct.pack_into('<H',mem[oi],pos,tgt&0xffff); nfix+=1
    return objs,mem,nfix
if __name__=='__main__':
    objs,mem,n=load_le(sys.argv[1])
    print('objs',[(hex(o[1]),hex(o[0]),hex(o[2])) for o in objs],'fixups',n)
    segs=[(o[1],bytes(mem[i][:o[0]] if o[0] else mem[i])) for i,o in enumerate(objs) if o[0]]
    # write ELF32
    hdr=bytearray(52); ph=32*len(segs); off=52+ph
    out=bytearray(); phs=bytearray(); data=bytearray()
    cur=(off+0xfff)&~0xfff
    body=[]
    for va,b in segs:
        phs+=struct.pack('<8I',1,cur,va,va,len(b),len(b),7,0x1000); body.append((cur,b)); cur=(cur+len(b)+0xfff)&~0xfff
    hdr[0:16]=b'\x7fELF\x01\x01\x01'+b'\0'*9
    struct.pack_into('<HHIIIIIHHHHHH',hdr,16,2,3,1,0,52,0,0,52,32,len(segs),40,0,0)
    f=bytearray(cur); f[0:52]=hdr; f[52:52+ph]=phs
    for o,b in body: f[o:o+len(b)]=b
    open(sys.argv[2],'wb').write(f)
