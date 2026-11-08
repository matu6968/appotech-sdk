#!/usr/bin/env python3
"""The SDK's per-board bootstrap constants must match the vendor bootstraps they describe.

The SPI-boot entry restores the ROM stack page that the vendor bootstrap saved. Each camera's bootstrap loads the
save address with a `l.movhi r4,0 / l.ori r4,r4,ADDR` pair; this reads that pair out of the stock image and compares it
with APX_SPI_BOOT_SAVED_STACK in the board header, so a wrong constant (a silent reset loop on a cold boot) fails here."""
import re,struct,sys
from pathlib import Path
sdk=Path(__file__).resolve().parents[1];root=sdk.parent
BOARDS=[('action-vision',root/'flash.bin.golden',0x1e6c,0x540),
        ('silvergear-3222882',root/'stock-backups/silvergear-3222882-20261102/stock.bin',0x1c94,0x558)]
checked=0
for board,image,expect,offset in BOARDS:
    header=(sdk/'boards'/board/'board.h').read_text()
    constant=int(re.search(r'#define APX_SPI_BOOT_SAVED_STACK (0x[0-9a-fA-F]+)u',header).group(1),16)
    assert constant==expect,f'{board}: board.h has {constant:#x}, expected {expect:#x}'
    if not image.exists():
        print(f'{board}: stock image not present, constant only');continue
    data=image.read_bytes()
    movhi,ori=struct.unpack_from('<II',data,offset)
    assert movhi==0x18800000,f'{board}: SPI {offset:#x} is not l.movhi r4,0 ({movhi:#010x})'
    assert ori&0xffff0000==0xa8840000,f'{board}: SPI {offset:#x}+4 is not l.ori r4,r4,imm ({ori:#010x})'
    assert ori&0xffff==constant,f'{board}: bootstrap saves the stack page at {ori&0xffff:#x}, board.h says {constant:#x}'
    checked+=1
print(f'bootstrap layout ok: {checked} stock image(s) match the board headers')
