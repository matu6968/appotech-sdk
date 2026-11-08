#!/usr/bin/env python3
"""Compare the C ports in src/photo_prepare.c bit for bit with the real stock instructions
(tools/stock_photo_reference.py): sharpen 0206c6a4, CLAHE 020680c8, quantiser 020688a8,
and the Dot wrapper's size rules. Needs the stock application and its listing (../stock-backups,
research/silvergear/stock-app.disasm); skips with a message when they are absent."""
import ctypes,random,subprocess,sys,tempfile
from pathlib import Path
sdk=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(sdk/'tools'))
STOCK=sdk.parent/'stock-backups/silvergear-3222882-20261102/analysis/stock-app-02000000.bin'
LISTING=sdk/'research/silvergear/stock-app.disasm'
if not STOCK.exists() or not LISTING.exists():
    print('photo prepare test skipped: stock application or listing not present');sys.exit(0)
from stock_photo_reference import StockPhoto,ENHANCE
import struct

tmp=tempfile.TemporaryDirectory(prefix='jt5295-photo-')
so=Path(tmp.name)/'photo.so'
subprocess.run(['cc','-std=c11','-O2','-fPIC','-shared','-Wall','-Wextra','-Werror','-I'+str(sdk/'include'),
    str(sdk/'src/photo_prepare.c'),'-o',str(so)],check=True)
lib=ctypes.CDLL(str(so))
U8P=ctypes.POINTER(ctypes.c_uint8)
lib.apx_photo_sharpen.argtypes=[U8P,ctypes.c_uint,ctypes.c_uint,ctypes.c_int,ctypes.c_int,U8P,ctypes.c_size_t]
lib.apx_photo_clahe.argtypes=[U8P,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,
    ctypes.c_float,ctypes.POINTER(ctypes.c_uint32),ctypes.c_size_t]
lib.apx_photo_quantise.argtypes=[U8P,ctypes.c_uint,ctypes.c_uint,ctypes.c_uint]
lib.apx_photo_prepare.argtypes=[ctypes.c_int,U8P,ctypes.c_uint,ctypes.c_uint,ctypes.c_void_p,ctypes.c_size_t]
stock=StockPhoto(STOCK,LISTING)

def buf(data):return (ctypes.c_uint8*len(data)).from_buffer_copy(data)
def c_sharpen(img,w,h,strength,offset):
    a=buf(img);scratch=(ctypes.c_uint8*(w*h))()
    r=lib.apx_photo_sharpen(a,w,h,strength,offset,scratch,w*h);return r,bytes(a)
def c_clahe(img,w,h,nx,ny,bins,clip,low=0,high=255):
    a=buf(img);hist=(ctypes.c_uint32*(max(1,nx)*max(1,ny)*(bins or 128)))()
    r=lib.apx_photo_clahe(a,w,h,low,high,nx,ny,bins,clip,hist,ctypes.sizeof(hist));return r,bytes(a)
def c_quantise(img,w,h,levels):
    a=buf(img);lib.apx_photo_quantise(a,w,h,levels);return bytes(a)

def images(w,h,seed):
    rnd=random.Random(seed)
    noise=bytes(rnd.randrange(256) for _ in range(w*h))
    ramp=bytes(((x*255)//(w-1)) for y in range(h) for x in range(w))
    blocks=bytes(((x//8+y//8)%2*200+rnd.randrange(30))&255 for y in range(h) for x in range(w))
    dark=bytes(rnd.randrange(90) for _ in range(w*h))
    bright=bytes(160+rnd.randrange(96) for _ in range(w*h))
    flat=bytes([128])*(w*h)
    edges=bytes((230 if (x>w//3 and y>h//4) else 25)+rnd.randrange(8) for y in range(h) for x in range(w))
    return {'noise':noise,'ramp':ramp,'blocks':blocks,'dark':dark,'bright':bright,'flat':flat,'edges':edges}

def check(name,expected,got):
    if expected!=got:
        bad=[i for i,(a,b) in enumerate(zip(expected,got)) if a!=b]
        raise SystemExit(f'MISMATCH {name}: {len(bad)} bytes differ, first at {bad[0]} (stock {expected[bad[0]]}, port {got[bad[0]]})')

count=0
for w,h in ((48,32),(64,48),(80,48)):
    for iname,img in images(w,h,w*h).items():
        for strength,offset in ((4,0),(1,0),(5,7),(0,60),(9,-3)):
            r,exp=stock.prepare(img,w,h,strength,offset);rc,got=c_sharpen(img,w,h,strength,offset)
            assert r==0 and rc==0;check(f'sharpen {iname} {w}x{h} s={strength} o={offset}',exp,got);count+=1
        for levels in (16,2,4,17,64):
            _,exp=stock.quantise(img,w,h,levels);check(f'quantise {iname} {w}x{h} L={levels}',exp,c_quantise(img,w,h,levels));count+=1
for w,h,nx,ny,bins,clip in ((64,48,8,8,64,2.0),(48,32,4,4,64,1.5),(96,32,12,4,64,1.5),(80,48,10,10,64,2.0),
                            (64,48,8,8,32,3.0),(64,48,4,8,0,0.5),(64,48,8,4,64,6.5),(64,48,8,8,64,0.0),(64,48,8,8,64,-1.0)):
    for iname,img in images(w,h,w+h+nx).items():
        r,exp=stock.enhance(img,w,h,nx,ny,bins,clip);rc,got=c_clahe(img,w,h,nx,ny,bins,clip)
        if r!=(rc&0xffffffff):raise SystemExit(f'CLAHE return {iname} {w}x{h} {nx}x{ny}: stock {r:#x} port {rc}')
        check(f'clahe {iname} {w}x{h} {nx}x{ny} bins={bins} clip={clip}',exp,got);count+=1
# Rejected parameters leave the image untouched and return the same code.
img=images(64,48,1)['noise']
for args in ((64,48,3,8,64,2.0),(64,48,8,16,64,2.0),(64,48,1,8,64,2.0),(65,48,8,8,64,2.0),(64,48,8,8,64,1.0),(64,48,65,8,64,2.0),(64,48,8,65,64,2.0)):
    w,h,nx,ny,bins,clip=args
    if w*h!=len(img):img2=bytes(range(256))*((w*h)//256+1);img2=img2[:w*h]
    else:img2=img
    r,exp=stock.enhance(img2,w,h,nx,ny,bins,clip);rc,got=c_clahe(img2,w,h,nx,ny,bins,clip)
    if r!=(rc&0xffffffff):raise SystemExit(f'reject return {args}: stock {r:#x} port {rc}')
    check(f'reject {args}',exp,got);count+=1
# The Gray pipeline in the port equals sharpen(4,0) -> clahe(12x12,64,1.5 where accepted) -> quantise(16) on the stock stages.
for w,h in ((96,48),(48,32)):
    img=images(w,h,5)['blocks']
    r,s1=stock.prepare(img,w,h,4,0);r2,s2=stock.enhance(s1,w,h,12,12,64,1.5);s3=s2 if r2!=0 else s2
    _,exp=stock.quantise(s3,w,h,16)
    a=buf(img);scratch=(ctypes.c_uint8*(max(w*h,36864)+64))()
    rc=lib.apx_photo_prepare(0,a,w,h,ctypes.addressof(scratch),ctypes.sizeof(scratch));assert rc==0
    check(f'gray pipeline {w}x{h} (clahe rc {r2:#x})',exp,bytes(a));count+=1
# Dot wrapper size rules match the stock dispatch.
for (w,h),expected in (((512,384),8),((672,384),8),((384,672),8),((384,216),8),((216,384),8),((320,180),10),((320,240),10),((640,480),0),((256,192),0)):
    seen=[c for c in stock.wrapper_calls(0x02053e0c,w,h) if c[0]==ENHANCE]
    got=seen[0][1][5] if seen else 0
    assert got==expected,((w,h),got,expected);count+=1
print(f'Photo prepare model: {count} comparisons with the stock sharpen, CLAHE, quantiser and Dot wrapper instructions are bit exact')
