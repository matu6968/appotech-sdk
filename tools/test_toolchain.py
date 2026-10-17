#!/usr/bin/env python3
"""Native endian/relocation checks and generic OpenRISC regression smoke test."""
import json
from pathlib import Path
import subprocess
from elftools.elf.elffile import ELFFile
SDK=Path(__file__).resolve().parents[1]
CROSS=SDK/'toolchain/install/bin/or1k-appotech-elf-'
OUT=SDK/'build/toolchain-check'
def run(tool,*args):
    return subprocess.check_output([str(CROSS)+tool,*map(str,args)],text=True)
def main():
    OUT.mkdir(exist_ok=True)
    source=OUT/'reloc.S'
    source.write_text('''.section .text
.global _start
_start:
 l.movhi r3,hi(target)
 l.ori r3,r3,lo(target)
 l.jal target
 l.nop
target:
 l.jr r9
 l.nop
.section .data
ptr: .long target
 .short 0x1234
 .byte 0x56,0x78
 .quad 0x0123456789abcdef
 .float -1.25
 .double 2.5
''')
    results={}
    for name,endian in [('native','-EL'),('generic','-EB')]:
        obj=OUT/f'{name}.o';elfpath=OUT/f'{name}.elf'
        run('as',endian,'-o',obj,source)
        run('ld',endian,'-Ttext=0x02180000','-Tdata=0x02181000','-e','_start','-o',elfpath,obj)
        with elfpath.open('rb') as stream:
            elf=ELFFile(stream);order='little' if name=='native' else 'big'
            assert elf.little_endian==(order=='little')
            text=elf.get_section_by_name('.text').data();data=elf.get_section_by_name('.data').data()
            target=0x02180010
            words=[int.from_bytes(text[i:i+4],order) for i in range(0,len(text),4)]
            assert words[:3]==[0x18600218,0xa8630010,0x04000002],words
            assert int.from_bytes(data[:4],order)==target
            assert data[4:6]==(0x1234).to_bytes(2,order)
            assert int.from_bytes(data[8:16],order)==0x0123456789abcdef
            assert int.from_bytes(data[16:20],order)==0xbfa00000
            assert int.from_bytes(data[20:28],order)==0x4004000000000000
            disasm=run('objdump','-dr',elfpath)
            assert 'l.movhi r3,0x218' in disasm and 'l.jal' in disasm
            results[name]={'endianness':order,'relocations':['HI16','LO16','REL26','ABS32'],'disassembly':'passed'}
    c=OUT/'endian.c';c.write_text('unsigned value=0x12345678; unsigned f(unsigned x){return x*37+x/7;}')
    for name,flags in [('native',['-mappotech']),('generic',[])]:
        obj=OUT/f'{name}-c.o';run('gcc',*flags,'-O2','-c',c,'-o',obj)
        with obj.open('rb') as stream:
            elf=ELFFile(stream);assert elf.little_endian==(name=='native')
            assert bool(elf.header.e_flags&1)==(name=='native')
    print(json.dumps(results,indent=2))
    (SDK/'build/toolchain-check.json').write_text(json.dumps(results,indent=2)+'\n')
if __name__=='__main__':main()
