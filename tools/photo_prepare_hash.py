#!/usr/bin/env python3
"""Run src/photo_prepare.c on the host over a decoded luma dump and print the FNV32 the demo logs
("prepared luma FNV32=...") plus an optional PNG, so a camera run can be checked against the host.

  apxtool debug read 0x<luma address> <pitch*height> --output luma.bin
  photo_prepare_hash.py luma.bin --width 512 --height 384 --pitch 512 --mode gray|dot [--png out.png]"""
import argparse,ctypes,subprocess,tempfile
from pathlib import Path
sdk=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('dump',type=Path);p.add_argument('--width',type=int,required=True);p.add_argument('--height',type=int,required=True)
p.add_argument('--pitch',type=int,required=True);p.add_argument('--mode',choices=('gray','dot'),required=True);p.add_argument('--png',type=Path)
a=p.parse_args()
with tempfile.TemporaryDirectory() as d:
    so=Path(d)/'photo.so'
    subprocess.run(['cc','-std=c11','-O2','-fPIC','-shared','-I'+str(sdk/'include'),str(sdk/'src/photo_prepare.c'),'-o',str(so)],check=True)
    lib=ctypes.CDLL(str(so))
    raw=a.dump.read_bytes()
    packed=b''.join(raw[y*a.pitch:y*a.pitch+a.width] for y in range(a.height))
    img=(ctypes.c_uint8*len(packed)).from_buffer_copy(packed);scratch=(ctypes.c_uint8*len(packed))()
    lib.apx_photo_prepare.argtypes=[ctypes.c_int,ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_void_p,ctypes.c_size_t]
    r=lib.apx_photo_prepare(0 if a.mode=='gray' else 1,ctypes.addressof(img),a.width,a.height,ctypes.addressof(scratch),len(packed))
    out=bytes(img)
def fnv(b):
    h=2166136261
    for x in b:h=((h^x)*16777619)&0xffffffff
    return h
print(f'mode {a.mode} result {r} source FNV32={fnv(packed):08x} prepared FNV32={fnv(out):08x}')
if a.png:
    from PIL import Image
    Image.frombytes('L',(a.width,a.height),out).save(a.png)
