#!/usr/bin/env python3
"""Host-only execution of verified stock image-processing instructions.

No camera access. Only the stock Dot renderer's allocator/free calls are hooked;
its arithmetic, branches and memory accesses execute from the native listing.
"""
import hashlib
import re
from pathlib import Path
from silvergear_spr_ledger import APP_BASE,APP_SHA256,load,verify_listing

MASK=0xffffffff
def signed(x):return x-0x100000000 if x&0x80000000 else x
def truncdiv(x,y):return (abs(x)//abs(y))*(-1 if (x<0)!=(y<0) else 1)

class StockDot:
    def __init__(self,application,listing):
        self.image=Path(application).read_bytes()
        if hashlib.sha256(self.image).hexdigest()!=APP_SHA256:raise ValueError('stock app hash mismatch')
        verify_listing(listing,self.image)
        self.code=load(listing)
    def render(self,source,width,height,threshold=112):
        if len(source)!=width*height or not width or not height:raise ValueError('invalid image')
        frame=0x04000000;heap=0x04100000;stack=0x04200000;stop=0xfffffff0
        regions=[(APP_BASE,self.image),(frame,bytearray(source)),
                 (heap,bytearray(width*height*4)),(stack,bytearray(4096))]
        def read(address,n):
            for base,data in regions:
                if base<=address and address+n<=base+len(data):return int.from_bytes(data[address-base:address-base+n],'little')
            raise ValueError('stock read outside regions '+hex(address))
        def write(address,value,n):
            for base,data in regions[1:]:
                if base<=address and address+n<=base+len(data):
                    data[address-base:address-base+n]=(value&((1<<(n*8))-1)).to_bytes(n,'little');return
            raise ValueError('stock write outside regions '+hex(address))
        r=[0]*32;r[1]=stack+4096;r[3:7]=[frame,width,height,threshold];r[9]=stop
        pc=0x02068d58;flag=False
        def reg(a):return int(a[1:])
        def operand(a):return r[reg(a)] if a.startswith('r') else int(a,0)&MASK
        def address(a):
            m=re.fullmatch(r'(-?\d+)\(r(\d+)\)',a)
            if not m:raise ValueError('memory operand '+a)
            return (r[int(m[2])]+int(m[1]))&MASK
        for steps in range(20_000_000):
            if pc==stop:return bytes(regions[1][1])
            op,a=self.code[pc];nxt=pc+4
            if op=='l.jal':
                target=int(a[0],0)
                if target==0x0203b4e4:
                    if r[3]!=width*height*4:raise ValueError('unexpected stock allocation')
                    r[11]=heap
                elif target==0x0203b614:
                    if r[3]!=heap:raise ValueError('unexpected stock free')
                else:raise ValueError('unexpected stock call '+hex(target))
            elif op=='l.jr':nxt=r[reg(a[0])]
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
        raise ValueError('stock renderer instruction limit')
