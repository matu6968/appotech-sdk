#!/usr/bin/env python3
"""Prepare 0:/PRINT.JPG for examples/printer-sd-demo and printer-sd-compare, and check a luma dump.

  prepare_print_jpeg.py prepare SOURCE OUTPUT [--convert] [--upload [--port P]]
      Default copies SOURCE byte for byte (use this for the stock reference image.jpg).
      --convert re-encodes any image as baseline 4:2:0 quality 80, fitted inside 640x480.
      Rejects what the hardware decoder cannot take (progressive, 4:4:4, grayscale, >2048x1536).
  prepare_print_jpeg.py verify SOURCE DUMP --width W --height H --pitch P [--png OUT]
      Compare the decoded luma read back from the camera with a host decode of the same file:
        apxtool debug read 0x<luma address from the log> <pitch*height> --output dump.bin
      Prints the mean absolute error and PSNR; a correct hardware decode is close (small error),
      a wrong pitch or garbled decode is not. Nothing here touches the SD card except --upload. """
import argparse,hashlib,json,shutil,struct,sys
from pathlib import Path

def sof(data):
    """Return (marker, height, width, [(id, sampling, quant)]) of the first SOF, or None."""
    if data[:2]!=b'\xff\xd8':return None
    i=2
    while i+4<=len(data):
        if data[i]!=0xff:return None
        m=data[i+1]
        if m==0xff:i+=1;continue
        length=struct.unpack('>H',data[i+2:i+4])[0]
        if 0xc0<=m<=0xcf and m not in (0xc4,0xc8,0xcc):
            h,w,n=struct.unpack('>HHB',data[i+5:i+10])
            return m,h,w,[(data[i+10+3*k],data[i+11+3*k],data[i+12+3*k]) for k in range(n)]
        if m==0xda:return None
        i+=2+length
    return None

def check_hardware_compatible(data):
    info=sof(data)
    if info is None:raise SystemExit('not a JPEG with a frame header')
    marker,h,w,comps=info
    if marker!=0xc0:raise SystemExit(f'frame marker {marker:#x}: only baseline (SOF0) is accepted by the hardware decoder')
    if len(comps)!=3:raise SystemExit('hardware decoder needs three components')
    if comps[0][1] not in (0x21,0x22) or comps[1][1]!=0x11 or comps[2][1]!=0x11:
        raise SystemExit(f'sampling {[hex(c[1]) for c in comps]}: only 4:2:0 and 4:2:2 are accepted')
    if w>2048 or h>1536:raise SystemExit(f'{w}x{h} exceeds the 2048x1536 source limit; use --convert')
    if len(data)>576*1024-32:raise SystemExit('file larger than the example stream buffer (576 KiB - 32)')
    return w,h,'4:2:0' if comps[0][1]==0x22 else '4:2:2'

def prepare(a):
    source=a.source.read_bytes()
    if a.convert:
        from PIL import Image,ImageOps
        im=ImageOps.exif_transpose(Image.open(a.source)).convert('RGB')
        im.thumbnail((640,480),Image.LANCZOS)
        im=im.resize((im.width&~1 or 2,im.height&~1 or 2),Image.LANCZOS)
        a.output.parent.mkdir(parents=True,exist_ok=True)
        im.save(a.output,'JPEG',quality=80,subsampling=2,progressive=False)
    else:
        a.output.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(a.source,a.output)
    data=a.output.read_bytes();w,h,sub=check_hardware_compatible(data)
    row={'source':str(a.source),'source_sha256':hashlib.sha256(source).hexdigest(),'output':str(a.output),
         'output_sha256':hashlib.sha256(data).hexdigest(),'bytes':len(data),'size':[w,h],'sampling':sub,'converted':a.convert}
    print(json.dumps(row))
    if a.upload:
        from apxtool.debug import DebugClient,USBTransport
        from apxtool.storage import StorageClient
        with USBTransport(port=a.port) as t:
            with a.output.open('rb') as f:StorageClient(DebugClient(t)).upload('0:/PRINT.JPG',f)
        print('uploaded to 0:/PRINT.JPG')

def verify(a):
    from PIL import Image
    import numpy as np
    dump=a.dump.read_bytes()
    if len(dump)<a.pitch*a.height:raise SystemExit('dump shorter than pitch*height')
    luma=np.frombuffer(dump[:a.pitch*a.height],dtype=np.uint8).reshape(a.height,a.pitch)[:,:a.width]
    if a.png:Image.fromarray(luma).save(a.png)
    ref=Image.open(a.source).convert('L').resize((a.width,a.height),Image.LANCZOS)
    ref=np.asarray(ref,dtype=np.float64);got=luma.astype(np.float64)
    mae=float(np.abs(ref-got).mean());mse=float(((ref-got)**2).mean())
    psnr=99.0 if mse==0 else min(99.0,10*np.log10(255**2/mse))
    print(json.dumps({'width':a.width,'height':a.height,'pitch':a.pitch,'mae':round(mae,2),'psnr_db':round(float(psnr),2),
                      'verdict':'plausible hardware decode' if psnr>=25 else 'MISMATCH: check pitch/garbling before printing'}))
    return 0 if psnr>=25 else 1

def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    s=p.add_subparsers(dest='command',required=True)
    q=s.add_parser('prepare');q.add_argument('source',type=Path);q.add_argument('output',type=Path)
    q.add_argument('--convert',action='store_true');q.add_argument('--upload',action='store_true');q.add_argument('--port')
    v=s.add_parser('verify');v.add_argument('source',type=Path);v.add_argument('dump',type=Path)
    for n in ('width','height','pitch'):v.add_argument('--'+n,type=int,required=True)
    v.add_argument('--png',type=Path)
    a=p.parse_args()
    if a.command=='prepare':prepare(a);return 0
    return verify(a)
if __name__=='__main__':sys.exit(main())
