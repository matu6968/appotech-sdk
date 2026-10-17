#!/usr/bin/env python3
"""Packs a synthetic image with the real packer, then runs src/spi_overlay.c against it on the host."""
import hashlib,struct,subprocess,sys,tempfile
from pathlib import Path
sdk=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(sdk.parent/'apxtool/src'))
from apxtool import spi_boot
from apxtool.sdk_flash import CAPACITY
with tempfile.TemporaryDirectory(prefix='jt5295-overlay-') as directory:
    d=Path(directory)
    data=bytearray((i*37)&255 for i in range(CAPACITY));struct.pack_into('<I',data,0x24,17)
    data[spi_boot.OVERLAY_TABLE:spi_boot.OVERLAY_TABLE+4096]=b'\xff'*4096
    original=bytes(data)
    spi_boot.PREFIX_SHA256=hashlib.sha256(original[:0x2200]).hexdigest()
    a=bytes((i*7+3)&255 for i in range(100003));b=b'ASSET'*4001
    spi_boot.load_manifest=lambda p:({'spi_entry':0x02180010,'build_id':'model'},
        [(0x02180000,b'CODE'*100),(0x02220000,b'T'),(0x022e0000,a),(0x02300000,b)])
    (d/'m.json').write_text('{}')
    image,_=spi_boot.build_image(original,d/'m.json')
    (d/'flash.bin').write_bytes(image);(d/'a.bin').write_bytes(a);(d/'b.bin').write_bytes(b)
    binary=d/'model'
    subprocess.run(['cc','-std=gnu11','-O1','-no-pie','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DAPX_OVERLAY_PTR(address)=(bank+((address)-0x022e0000u))','-include',str(sdk/'tests/spi_overlay_model.h'),
                    '-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),
                    str(sdk/'src/spi_overlay.c'),str(sdk/'tests/spi_overlay_model.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary),str(d/'flash.bin'),str(d/'a.bin'),str(d/'b.bin')],check=True)
