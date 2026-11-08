#!/usr/bin/env python3
"""Execute the real SRAM stage's established OR1K subset, with DDR calibration stubbed.

Checks padding/control flow, PC+4 links, DDR clearing, SRAM restoration, ADC
calibration publication, field preservation and the non-returning ROM handoff.
This is a CPU/control-flow model, not a model of the analog DDR calibration.
"""
from pathlib import Path
import struct
sdk=Path(__file__).resolve().parents[1]
code=(sdk/'build/flasher/silvergear-stage.bin').read_bytes()
mem=bytearray((i*73+19)&255 for i in range(0x5000))
original=bytes(mem[0x3e8:0x2000])
mem[0x2000:0x3c18]=original
mem[0x3c20:0x3c20+len(code)]=code
struct.pack_into('<5I',mem,0x3e00,0x47535841,0xffffffff,0,0,0)
ddr=bytearray(b'\xa5'*0x400000)
regs=[0]*32;regs[1]=0x4e00;regs[3]=0x3e00;regs[9]=0x1024ac
regs[14]=0x12345678;regs[18]=0x87654321
spr={0x18:0xf0004006,0x4800:0xabcdffff,0xa004:0x40}
pc=0x3c20;flag=False;calibrated=detached=False
def signed(value,bits):return value-(1<<bits) if value&(1<<(bits-1)) else value
def read(address):
    assert 0<=address<=len(mem)-4
    return struct.unpack_from('<I',mem,address)[0]
def write(address,value):
    if 0x02000000<=address<=0x023ffffc:
        assert calibrated
        struct.pack_into('<I',ddr,address-0x02000000,value)
    else:
        assert 0<=address<=len(mem)-4
        struct.pack_into('<I',mem,address,value)
for steps in range(8000000):
    if pc==0x10385c:break
    if pc==0x10fc:
        assert detached
        calibrated=True;write(0x1e14,800);pc=regs[9];continue
    word=read(pc);op=word>>26;d=(word>>21)&31;a=(word>>16)&31;b=(word>>11)&31
    imm=word&0xffff;split=((word>>10)&0xf800)|(word&0x7ff);next_pc=pc+4
    if op==6:regs[d]=imm<<16
    elif op==39:regs[d]=(regs[a]+signed(imm,16))&0xffffffff
    elif op==42:regs[d]=regs[a]|imm
    elif op==33:regs[d]=read((regs[a]+signed(imm,16))&0xffffffff)
    elif op==53:write((regs[a]+signed(split,16))&0xffffffff,regs[b])
    elif op==45:regs[d]=spr.get(regs[a]|imm,0)
    elif op==48:
        index=regs[a]|split;spr[index]=regs[b]
        if index==0xa004 and not regs[b]&0x40:detached=True
    elif op==56 and word&0x7ff==3:regs[d]=regs[a]&regs[b]
    elif op==47 and d==1:flag=regs[a]!=(signed(imm,16)&0xffffffff)
    elif op==57 and d==4:flag=regs[a]<regs[b]
    elif op in (0,4):
        target=pc+signed(word&0x3ffffff,26)*4
        if op==0 or flag:
            assert target!=pc,f'infinite branch/padding at {pc:#x}'
            next_pc=target
    elif op in (17,18):
        next_pc=regs[b]
        if op==18:regs[9]=pc+4
    elif op!=5:raise AssertionError(f'unhandled instruction {word:08x} at {pc:#x}')
    regs[0]=0;pc=next_pc
else:raise AssertionError('stage failed to reach the ROM initializer')
expected=bytearray(original);struct.pack_into('<I',expected,0x1e14-0x3e8,800)
assert mem[0x3e8:0x2000]==expected and not any(ddr)
assert regs[1]==0x4e00 and regs[14]==0x12345678 and regs[18]==0x87654321
assert spr[0x18]==0xf0000000 and spr[0x4800]==0xabcd7fff
assert spr[0x8858]==0x1623 and spr[0x885c]==0x0b71b000
assert read(0x3e04)==0 and read(0x3e10)==800
print(f'Silvergear stage model: {steps+1} instructions; no padding trap, DDR clear, SRAM restore and ROM handoff passed')
