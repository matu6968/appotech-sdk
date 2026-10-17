#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,sys,json
import numpy as np
from PIL import Image
sdk=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='jt5295-image-') as directory:
    directory=Path(directory);header=directory/'stb_image_sdk.h'
    subprocess.run([sys.executable,str(sdk/'tools/prepare_stb.py'),str(header)],check=True)
    binary=directory/'image-model'
    subprocess.run(['cc','-std=c11','-O2','-no-pie','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation',
                    '-fsanitize=address,undefined','-I'+str(sdk/'tests/mock'),'-I'+str(sdk/'include'),'-I'+str(sdk/'third_party/fatfs'),'-I'+str(directory),
                    str(sdk/'src/image.c'),str(sdk/'tests/image_model.c'),'-o',str(binary)],check=True)
    source=Image.open(sdk/'tests/fixtures/jpeg-baseline-220x176.jpg').convert('RGB')
    paths=[]
    for width,height in [(220,176),(320,240),(640,480),(1280,720),(1600,1200)]:
        p=directory/f'baseline-{width}x{height}.jpg';source.resize((width,height)).save(p,quality=90,subsampling=2,progressive=False);paths.append(p)
    for fmt in ['PNG','BMP','TGA','GIF','PPM']:
        p=directory/('test.'+fmt.lower());source.save(p,format=fmt);paths.append(p)
    flat=directory/'flat.gif';Image.new('RGB',(220,176),(23,75,188)).save(flat);paths.append(flat)
    for sampling in [0,1,2]:
        p=directory/f'progressive-{sampling}.jpg';source.save(p,quality=100,subsampling=sampling,progressive=True);paths.append(p)
        p=directory/f'baseline-{sampling}.jpg';source.save(p,quality=100,subsampling=sampling);paths.append(p)
    p=directory/'normal-quality.jpg';source.save(p,quality=75,progressive=True);paths.append(p)
    gray=directory/'gray.jpg';source.convert('L').save(gray,progressive=True);paths.append(gray)
    png16=directory/'gray16.png';Image.fromarray(np.arange(220*176,dtype=np.uint16).reshape(176,220)).save(png16);paths.append(png16)
    for n,path in enumerate(paths):
        target=directory/f'out{n}.nv21';arena=1024*1024 if path.parent==directory else 1024*1024-path.stat().st_size-32
        if path.name=='progressive-0.jpg':arena=65536 # force compressed SD paging for Q100
        subprocess.run([str(binary),str(path),str(target),str((arena//32)*32)],check=True)
        image=Image.open(path)
        if path==png16:image=Image.fromarray((np.asarray(image,dtype=np.uint16)>>8).astype(np.uint8))
        image=image.convert('RGB');sw,sh=image.size
        if sw*176>sh*220:w,h=220,(220*sh//sw)&~1
        else:h,w=176,(176*sw//sh)&~1
        if w>sw or h>sh:w,h=sw&~1,sh&~1
        pitch=(w+31)&~31;data=np.frombuffer(target.read_bytes(),np.uint8)
        y=data[:pitch*h].reshape(h,pitch)[:,:w].astype(float);vu=data[pitch*h:].reshape(h//2,pitch)[:,:w].astype(float)-128
        v=vu[:,::2].repeat(2,0).repeat(2,1);u=vu[:,1::2].repeat(2,0).repeat(2,1)
        rgb=np.clip(np.stack([y+1.402*v,y-.344136*u-.714136*v,y+1.772*u],2),0,255)
        ref=np.array(image.resize((w,h),Image.Resampling.BILINEAR),dtype=float);mae=np.abs(rgb-ref).mean()
        assert mae<22,(path,mae);print(path.name,'RGB MAE',round(mae,3),flush=True)
print('Software image model passed: five synthetic baseline JPEG sizes, progressive 420/422/444, Q100, grayscale, PNG/BMP/TGA/GIF/PNM, paging, cleanup and bounds')
