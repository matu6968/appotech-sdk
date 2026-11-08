#!/usr/bin/env python3
"""Host-only execution of the verified stock photo-preparation instructions.

Runs the hash-verified stock application's own code (sharpen 0206c6a4, tiled histogram
equalisation 020680c8, 16-level quantiser 020688a8, with its soft-float helpers) on host memory,
so a C port can be compared bit for bit. No camera access. Only the stock allocator and free are
hooked; no delay slots (the native listing is authoritative, see printer-modes-20261107.md, archived in cleanup-archive-20261108).
"""
import hashlib
import re
import struct
from pathlib import Path
from silvergear_spr_ledger import APP_BASE,APP_SHA256,load,verify_listing

MASK=0xffffffff
ALLOC=0x0203b4e4
FREE=0x0203b614
PREPARE=0x0206c6a4
ENHANCE=0x020680c8
FILTER=0x020688a8
def signed(x):return x-0x100000000 if x&0x80000000 else x
def truncdiv(x,y):return (abs(x)//abs(y))*(-1 if (x<0)!=(y<0) else 1)

class StockCPU:
    def __init__(self,application,listing):
        self.image=Path(application).read_bytes()
        if hashlib.sha256(self.image).hexdigest()!=APP_SHA256:raise ValueError('stock app hash mismatch')
        verify_listing(listing,self.image)
        self.code=load(listing)
        self.steps=0
    def call(self,entry,args,buffers,stack_args=(),heap_bytes=1<<20,max_steps=400_000_000,hooks=None):
        """buffers: {name: (address, bytearray)} writable host memory. Returns (r11, buffers)."""
        heap=0x05000000;stack=0x06000000;stop=0xfffffff0
        regions=[(APP_BASE,self.image,False),(heap,bytearray(heap_bytes),True),(stack,bytearray(8192),True)]
        for base,data in buffers.values():regions.append((base,data,True))
        bump=[heap]
        def find(address,n,write):
            for base,data,writable in regions:
                if base<=address and address+n<=base+len(data):
                    if write and not writable:raise ValueError('stock write to read-only %#x'%address)
                    return base,data
            raise ValueError('stock access outside regions %#x'%address)
        def read(address,n):
            base,data=find(address,n,False);return int.from_bytes(data[address-base:address-base+n],'little')
        def write(address,value,n):
            base,data=find(address,n,True);data[address-base:address-base+n]=(value&((1<<(n*8))-1)).to_bytes(n,'little')
        r=[0]*32;sp=stack+8192-64;r[1]=sp
        for i,a in enumerate(args[:6]):r[3+i]=a&MASK
        for i,a in enumerate(stack_args):write(sp+4*i,a&MASK,4)
        r[9]=stop;pc=entry;flag=False
        def reg(a):return int(a[1:])
        def operand(a):return r[reg(a)] if a.startswith('r') else int(a,0)&MASK
        def address(a):
            m=re.fullmatch(r'(-?\d+)\(r(\d+)\)',a)
            if not m:raise ValueError('memory operand '+a)
            return (r[int(m[2])]+int(m[1]))&MASK
        code=self.code
        for steps in range(max_steps):
            if pc==stop:
                self.steps=steps;return r[11],buffers
            if pc not in code:raise ValueError('stock pc outside listing %#x'%pc)
            op,a=code[pc];nxt=pc+4
            if op=='l.jal':
                target=int(a[0],0)
                if target==ALLOC:
                    size=(r[3]+31)&~31;r[11]=bump[0];bump[0]+=size
                    if bump[0]>heap+heap_bytes:raise ValueError('stock heap exhausted')
                elif target==FREE:pass
                elif hooks and target in hooks:
                    # Observe the call (registers and the three stack arguments) without executing it.
                    r[11]=hooks[target]([r[3],r[4],r[5],r[6],r[7],r[8]],[read(r[1]+4*i,4) for i in range(3)])&MASK
                else:r[9]=nxt;nxt=target
            elif op=='l.jr':nxt=r[reg(a[0])]
            elif op=='l.jalr':t=r[reg(a[0])];r[9]=nxt;nxt=t
            elif op=='l.j':nxt=int(a[0],0)
            elif op in ('l.bf','l.bnf'):
                if flag==(op=='l.bf'):nxt=int(a[0],0)
            elif op.startswith('l.sf'):
                kind=op[4:];immediate=kind.endswith('i');kind=kind[:-1] if immediate else kind
                x,y=operand(a[0]),operand(a[1]);sx,sy=signed(x),signed(y)
                flag={'eq':x==y,'ne':x!=y,'ltu':x<y,'leu':x<=y,'gtu':x>y,'geu':x>=y,
                      'lts':sx<sy,'les':sx<=sy,'gts':sx>sy,'ges':sx>=sy}[kind]
            elif op in ('l.lwz','l.lbz','l.lbs','l.lhz','l.lhs'):
                n={'l.lwz':4,'l.lbz':1,'l.lbs':1,'l.lhz':2,'l.lhs':2}[op];v=read(address(a[1]),n)
                if op in ('l.lbs','l.lhs') and v&(1<<(n*8-1)):v-=1<<(n*8)
                r[reg(a[0])]=v&MASK
            elif op in ('l.sw','l.sb','l.sh'):write(address(a[0]),operand(a[1]),{'l.sw':4,'l.sb':1,'l.sh':2}[op])
            elif op=='l.movhi':r[reg(a[0])]=(int(a[1],0)<<16)&MASK
            else:
                x,y=operand(a[1]),operand(a[2]);kind=op[2:]
                if kind in ('add','addi'):v=x+y
                elif kind=='sub':v=x-y
                elif kind in ('and','andi'):v=x&y
                elif kind in ('or','ori'):v=x|y
                elif kind in ('xor','xori'):v=x^y
                elif kind in ('sll','slli'):v=x<<(y&31)
                elif kind in ('srl','srli'):v=x>>(y&31)
                elif kind in ('sra','srai'):v=signed(x)>>(y&31)
                elif kind in ('mul','muli'):v=x*y
                elif kind=='div':v=truncdiv(signed(x),signed(y))
                elif kind=='divu':v=x//y
                else:raise ValueError('unsupported instruction '+op)
                r[reg(a[0])]=v&MASK
            r[0]=0;pc=nxt
        raise ValueError('stock photo stage instruction limit')

class StockPhoto(StockCPU):
    FRAME=0x04000000
    def run(self,entry,frame,w,h,extra,stack_args=()):
        # Stock buffers are NV12 frames: the quantiser's error diffusion spills below the last row
        # into the chroma plane, so give it slack and return only the luma region.
        buf=bytearray(frame)+bytearray(8*w+64)
        r11,_=self.call(entry,[self.FRAME,w,h,*extra],{'frame':(self.FRAME,buf)},stack_args)
        return r11,bytes(buf[:len(frame)])
    def prepare(self,frame,w,h,strength=4,offset=0):
        return self.run(PREPARE,frame,w,h,[strength,offset])
    def enhance(self,frame,w,h,nx,ny,bins=64,clip=1.5,low=0,high=255):
        bits=struct.unpack('>I',struct.pack('>f',clip))[0]
        return self.run(ENHANCE,frame,w,h,[low,high,nx],[ny,bins,bits])
    def quantise(self,frame,w,h,levels=16):
        return self.run(FILTER,frame,w,h,[levels])

    def wrapper_calls(self,entry,w,h,hooked=(ENHANCE,0x02068d58)):
        """Run a stock photo wrapper (buf,w,h) and record the calls it makes to the hooked stages."""
        seen=[]
        def make(target):
            def hook(regs,stack):seen.append((target,regs,stack));return 0
            return hook
        buf=bytearray(w*h+8*w+64)
        self.call(entry,[self.FRAME,w,h],{'frame':(self.FRAME,buf)},hooks={t:make(t) for t in hooked})
        return seen
