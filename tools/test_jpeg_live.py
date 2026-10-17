#!/usr/bin/env python3
"""Exercise the integrated JPEG driver on an already launched jpeg-demo.

Creates new files only under SDKJPEG. Requires Pillow, numpy and apxtool[usb].
Original jpeg-test files are read and hash checked, never rewritten.
"""
import argparse,hashlib,io,json,sys,time
from pathlib import Path
import numpy as np
from PIL import Image,JpegImagePlugin
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.debug import DebugClient,USBTransport,Operation
from apxtool.jpeg import JPEGClient
from apxtool.storage import StorageClient
from apxtool.gpio import GPIOClient
from apxtool.system import SystemClient
from apxtool.errors import ApxError,DebugCommandError

SIZES=[(220,176),(320,240),(640,480),(1280,720),(1280,960),(1600,1200),(1920,1080),(2048,1536)]
def sha(data):return hashlib.sha256(data).hexdigest()
def nv21(data,w,h,pitch):
    raw=np.frombuffer(data,dtype=np.uint8);y=raw[:pitch*h].reshape(h,pitch)[:,:w].astype(float)
    vu=raw[pitch*h:].reshape(h//2,pitch)[:,:w].astype(float)-128
    v=vu[:,::2].repeat(2,0).repeat(2,1);u=vu[:,1::2].repeat(2,0).repeat(2,1)
    return np.clip(np.stack((y+1.402*v,y-.344136*u-.714136*v,y+1.772*u),2),0,255).astype(np.uint8)
def pattern():
    bars=np.array([(255,0,0),(0,255,0),(0,0,255),(255,255,0),(0,255,255),(255,0,255),(255,255,255),(0,0,0)],dtype=int)
    rgb=bars[np.arange(220)*8//220][None,:,:].repeat(176,0)
    rgb[:4]=255;rgb[-4:]=255;rgb[:,:4]=255;rgb[:,-4:]=255
    for y in range(120,176):
        for x in range(16,80):rgb[y,x]=240 if (x//8+y//8)&1 else 16
    rgb[28:80,150:202]=(255,128,0)
    y=(77*rgb[:,:,0]+150*rgb[:,:,1]+29*rgb[:,:,2])>>8
    us=-43*rgb[:,:,0]-85*rgb[:,:,1]+128*rgb[:,:,2]
    vs=128*rgb[:,:,0]-107*rgb[:,:,1]-21*rgb[:,:,2]
    u=np.clip(128+(us.reshape(88,2,110,2).sum((1,3))>>10),0,255)
    v=np.clip(128+(vs.reshape(88,2,110,2).sum((1,3))>>10),0,255)
    frame=np.zeros((176+88,224),dtype=np.uint8);frame[:176,:220]=y
    frame[176:,:220:2]=v;frame[176:,1:220:2]=u
    return nv21(frame.tobytes(),220,176,224)
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--seconds',type=float,default=60)
    p.add_argument('--tag',default='',help='optional filename suffix for repeating a build without overwriting its prior test files')
    a=p.parse_args()
    if a.tag and (len(a.tag)>64 or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_' for c in a.tag)):
        p.error('tag must be 1..64 ASCII letters, digits, hyphen or underscore')
    a.output.mkdir(parents=True,exist_ok=False)
    sdk=Path(__file__).resolve().parents[1];report={'status':'running','decode':[],'encode':[],'roundtrip':[]}
    def checkpoint(): (a.output/'acceptance.json').write_text(json.dumps(report,indent=2)+'\n')
    try:
        with USBTransport(port=a.port) as t:
            d=DebugClient(t);j=JPEGClient(d);s=StorageClient(d);g=GPIOClient(d);system=SystemClient(d)
            report['sdk']=d.info();build=report['sdk']['build_id'];suffix=build+('-'+a.tag if a.tag else '')
            report['start_system']=system.info()
            def download(path):
                out=io.BytesIO();s.download(path,out);return out.getvalue()
            def preview(path,reference,tag):
                j.preview(path);i=j.wait();w,h=i['output'];pitch=i['pitch']
                frame=d.read(i['framebuffer'],pitch*h*3//2);rgb=nv21(frame,w,h,pitch)
                ref=np.asarray(reference.resize((w,h),Image.Resampling.BILINEAR),dtype=float)
                error=float(np.abs(rgb.astype(float)-ref).mean())
                assert i['source']==list(reference.size) and i['error']==0 and i['job_error']==0
                assert error<25,(tag,error)
                (a.output/(tag+'.nv21')).write_bytes(frame);Image.fromarray(rgb).save(a.output/(tag+'.png'))
                row={'path':path,'info':i,'rgb_mae':error,'dma_ms':i['ticks']*1000/32768,'frame_sha256':sha(frame)}
                print(tag,round(row['dma_ms'],2),'ms MAE',round(error,3),flush=True);return row
            for w,h in SIZES:
                name=f'jpeg-test-{w}x{h}.jpg';path='0:/SDKJPEG/BASELINE/'+name
                data=download(path);im=Image.open(io.BytesIO(data));im.load()
                assert im.size==(w,h) and JpegImagePlugin.get_sampling(im)==2
                row=preview(path,im.convert('RGB'),f'decode-{w}x{h}');row['file_sha256']=sha(data)
                report['decode'].append(row);checkpoint()
            for w,h in SIZES:
                path=f'0:/SDKJPEG/ENCODER/encode-{w}x{h}-{suffix}.jpg'
                j.encode_sample(path,w,h);i=j.wait();data=download(path)
                assert len(data)==i['bytes'] and data==d.read(i['stream'],i['bytes'])
                assert data[:2]==b'\xff\xd8' and data[-2:]==b'\xff\xd9'
                im=Image.open(io.BytesIO(data));im.load()
                assert im.size==(w,h) and JpegImagePlugin.get_sampling(im)==2 and not im.info.get('progressive')
                reference=Image.fromarray(pattern()).resize((w,h),Image.Resampling.BILINEAR)
                error=float(np.abs(np.asarray(im.convert('RGB'),dtype=float)-np.asarray(reference,dtype=float)).mean())
                assert error<18,(w,h,error)
                (a.output/f'encode-{w}x{h}.jpg').write_bytes(data);im.save(a.output/f'encode-{w}x{h}.png')
                report['encode'].append({'path':path,'info':i,'sha256':sha(data),'rgb_mae':error,'dma_ms':i['ticks']*1000/32768})
                print('encode',w,h,len(data),'bytes',round(error,3),'MAE',flush=True)
                report['roundtrip'].append(preview(path,im.convert('RGB'),f'roundtrip-{w}x{h}'));checkpoint()
            native=Image.open(sdk/'research/new-jpeg-inputs/jpeg-test-220x176.jpg').convert('RGB')
            out=io.BytesIO();native.save(out,'JPEG',quality=90,subsampling=1,progressive=False)
            path=f'0:/SDKJPEG/BASELINE/jpeg-test-422-{suffix}.jpg';s.upload(path,io.BytesIO(out.getvalue()))
            ref=Image.open(io.BytesIO(out.getvalue())).convert('RGB')
            report['decode_422']=preview(path,ref,'decode-422');checkpoint()
            rejection=[]
            def rejects(name,fn):
                before=j.info()['sequence']
                try:fn()
                except ApxError as e:rejection.append({'case':name,'error':str(e)})
                else:raise AssertionError('accepted '+name)
                assert j.info()['sequence']==before and d.info()['build_id']==build
            rejects('progressive_original',lambda:j.preview('0:/jpeg-test-220x176.jpg'))
            rejects('missing_file',lambda:j.preview('0:/SDKJPEG/no-such-file.jpg'))
            rejects('existing_encode_file',lambda:j.encode_sample(report['encode'][0]['path'],220,176))
            rejects('downscale_encode',lambda:j.encode_sample(f'0:/SDKJPEG/ENCODER/invalid-{suffix}.jpg',200,160))
            malformed=f'0:/SDKJPEG/BASELINE/truncated-{suffix}.jpg'
            baseline=download('0:/SDKJPEG/BASELINE/jpeg-test-220x176.jpg')
            s.upload(malformed,io.BytesIO(baseline[:-2]))
            rejects('missing_EOI',lambda:j.preview(malformed))
            rejects('malformed_APXD_payload',lambda:d.request(Operation.JPEG,address=2,length=1,payload=b'x'))
            report['rejected']=rejection
            j.preview('0:/SDKJPEG/BASELINE/jpeg-test-2048x1536.jpg');j.cancel();assert j.info()['job']==0
            report['cancel']=True
            start=time.monotonic();count=verified=0
            while time.monotonic()-start<a.seconds:
                row=report['decode'][count%len(SIZES)];j.preview(row['path']);j.wait()
                block=s.read(row['path'],0,4096);assert block==download(row['path'])[:len(block)]
                verified+=len(block);g.info();d.info();system.info();count+=1
            report['sustained']={'seconds':time.monotonic()-start,'decode_jobs':count,'verified_sd_bytes':verified}
            originals=[]
            for row in json.loads((sdk/'research/new-jpeg-inputs/inventory.json').read_text()):
                data=download('0:/'+row['source']);assert sha(data)==row['source_sha256']
                originals.append({'path':row['source'],'sha256':sha(data)})
            report['originals_unchanged']=originals;report['sd']=s.info();report['gpio']=g.info();report['system']=system.info()
            assert not report['sd']['errors'] and not report['gpio']['adc_timeouts']
            assert not any(d.read(0x02200000,512));report['stack_bottom_512_zero']=True
            j.preview('0:/SDKJPEG/BASELINE/jpeg-test-220x176.jpg');report['final_preview']=j.wait()
            report['logs']=d.logs().decode(errors='replace');report['status']='passed';checkpoint()
    except BaseException as e:report.update(status='failed',error=str(e));checkpoint();raise
    print(json.dumps({'status':report['status'],'sustained':report['sustained']},indent=2))
if __name__=='__main__':main()
