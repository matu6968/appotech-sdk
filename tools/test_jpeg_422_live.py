#!/usr/bin/env python3
"""Verify baseline 4:2:2 decode at every requested source size.

Creates separate files in SDKJPEG/BASELINE; exact existing copies can be reused.
"""
import argparse,hashlib,io,json,time
from pathlib import Path
from PIL import Image,JpegImagePlugin
import numpy as np
from test_jpeg_live import SIZES,nv21
from apxtool.debug import USBTransport,DebugClient
from apxtool.storage import StorageClient
from apxtool.jpeg import JPEGClient
from apxtool.errors import ApxError
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True)
p.add_argument('--output',type=Path,required=True);a=p.parse_args()
if a.output.exists():p.error('output must be new')
sdk=Path(__file__).resolve().parents[1];report={'status':'running','images':[]}
try:
 with USBTransport(port=a.port) as t:
  d=DebugClient(t);s=StorageClient(d);j=JPEGClient(d);report['sdk']=d.info();build=report['sdk']['build_id']
  for w,h in SIZES:
   source=sdk/f'research/new-jpeg-inputs/jpeg-test-{w}x{h}.jpg'
   image=Image.open(source).convert('RGB');out=io.BytesIO();image.save(out,'JPEG',quality=90,subsampling=1,progressive=False);data=out.getvalue()
   path=f'0:/SDKJPEG/BASELINE/jpeg-test-{w}x{h}-422-{build}.jpg'
   try:s.upload(path,io.BytesIO(data))
   except ApxError as e:
    if 'FR_EXIST' not in str(e):raise
    existing=io.BytesIO();s.download(path,existing);assert existing.getvalue()==data,'existing test file differs'
   j.preview(path);i=j.wait();ow,oh=i['output'];pitch=i['pitch']
   frame=d.read(i['framebuffer'],pitch*oh*3//2);rgb=nv21(frame,ow,oh,pitch)
   reference=Image.open(io.BytesIO(data));reference.load();assert JpegImagePlugin.get_sampling(reference)==1
   error=float(np.abs(rgb.astype(float)-np.asarray(reference.resize((ow,oh),Image.Resampling.BILINEAR),dtype=float)).mean())
   assert i['source']==[w,h] and not i['error'] and error<25,(w,h,error)
   capture=a.output.with_name(a.output.stem+f'-{w}x{h}.png');Image.fromarray(rgb).save(capture)
   report['images'].append({'path':path,'file_sha256':hashlib.sha256(data).hexdigest(),'info':i,'rgb_mae':error,'capture':str(capture)})
   print(w,h,round(error,3),'MAE',flush=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
  j.preview('0:/SDKJPEG/BASELINE/jpeg-test-220x176.jpg');report['final_preview']=j.wait();report['status']='passed'
except BaseException as e:report.update(status='failed',error=str(e));raise
finally:a.output.write_text(json.dumps(report,indent=2)+'\n')
