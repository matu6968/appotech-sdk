#!/usr/bin/env python3
"""Dump the SoC register ranges that configure pins, clocks, power and ADC, from a running SDK.

Used to diff a stock-launched camera against an SPI cold boot. Reads only
addresses the SDK itself reads; output is JSON {hex address: hex value}.
"""
import argparse,json,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'jl-uboot-tool'))
from apxtool.debug import DebugClient,USBTransport,Space
RANGES=[(0x8000,0x8220),(0x8340,0x8398),(0x8840,0x8870),(0x88dc,0x88f0),(0x9080,0x9098),(0x9170,0x917c),
        (0x9800,0x980c),(0x9840,0x9878),(0xa000,0xa008),(0xb000,0xb060),(0xd800,0xd850),(0x4800,0x4804),(0x18,0x1c)]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',required=True);p.add_argument('output',type=Path)
a=p.parse_args()
if a.output.exists():sys.exit(f'refusing to overwrite {a.output}')
client=DebugClient(USBTransport(port=a.port))
regs={}
for start,end in RANGES:
    for address in range(start,end,4):
        regs[f'{address:#06x}']=f'{int.from_bytes(client.read(address,4,space=Space.SPR,width=4),"little"):#010x}'
a.output.write_text(json.dumps(regs,indent=1)+'\n')
print(f'{len(regs)} registers -> {a.output}')
