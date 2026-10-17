#!/usr/bin/env python3
"""Microphone DMA, duplex, WAV saving and peripheral coexistence acceptance."""
import argparse,hashlib,io,json,math,struct,sys,time,wave
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.debug import USBTransport,DebugClient,Operation,Space
from apxtool.microphone import MicrophoneClient
from apxtool.audio import AudioClient
from apxtool.storage import StorageClient
from apxtool.jpeg import JPEGClient
from apxtool.gpio import GPIOClient
from apxtool.system import SystemClient
from apxtool.errors import ApxError
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True)
p.add_argument('--manifest',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--seconds',type=float,default=60);a=p.parse_args()
if a.output.exists():p.error('output must be new')
manifest=json.loads(a.manifest.read_text());report={'status':'running','rates':[],'boundaries':[],'negative':[],'duplex':[]}
def save():a.output.write_text(json.dumps(report,indent=2)+'\n')
def rejected(case,fn):
 try:fn()
 except ApxError as e:report['negative'].append({'case':case,'error':str(e)})
 else:raise AssertionError('accepted '+case)
def spectrum(data,rate,center):
 samples=struct.unpack('<'+str(len(data)//2)+'h',data)[rate//5::4]
 mean=sum(samples)/len(samples);samples=[v-mean for v in samples];fs=rate/4
 bins={f:abs(sum(v*complex(math.cos(2*math.pi*f*n/fs),math.sin(2*math.pi*f*n/fs)) for n,v in enumerate(samples))) for f in range(center-15,center+16)}
 return max(bins,key=bins.get),max(bins.values())/len(samples)
try:
 with USBTransport(port=a.port) as t:
  d=DebugClient(t);mic=MicrophoneClient(d);audio=AudioClient(d);sd=StorageClient(d);jpeg=JPEGClient(d);gpio=GPIOClient(d);system=SystemClient(d)
  info=d.info();assert info['build_id']==manifest['build_id'];report['sdk']=info
  audio.stop();mic.stop();audio.mute(True)
  clock=int.from_bytes(d.read(0x884c,4,space=Space.SPR,width=4),'little')
  for rate in [8000,16000,32000]:
   for trial in range(3):
    mic.start(rate*2,rate);i=mic.wait();measured=i['bytes']//2*32768/i['ticks']
    assert abs(measured/rate-1)<.01 and i['guard_ok'] and not i['timeouts']
    report['rates'].append({'rate':rate,'trial':trial+1,'measured':measured,'capture':i});print('rate',rate,round(measured,2),flush=True)
  for n in [32,64,96,512,4096,8192,65536]:
   mic.start(n,32000);i=mic.wait();data=mic.read(0,n)
   assert len(data)==n and i['guard_ok'] and data[-32:]!=bytes([0xa5])*32
   assert data==d.read(i['buffer'],n)
   report['boundaries'].append({'bytes':n,'sha256':hashlib.sha256(data).hexdigest(),'capture':i})
  mic.start(64000,16000)
  rejected('busy capture',lambda:mic.start())
  rejected('read while capturing',lambda:mic.read(0,32))
  rejected('save while capturing',lambda:mic.save('0:/SDKAudio/busy.wav'))
  mic.stop();assert mic.info()['state']==1
  rejected('read canceled capture',lambda:mic.read(0,32))
  for command,payload in [(1,b'x'),(1,struct.pack('<II',33,16000)),(1,struct.pack('<II',32,44100)),(1,struct.pack('<II',65568,16000)),(9,b'')]:
   rejected('malformed '+str(command)+' '+payload.hex(),lambda command=command,payload=payload:d.request(Operation.MICROPHONE,address=command,length=len(payload),payload=payload))
  mic.start(32000,16000);mic.wait()
  rejected('misaligned read',lambda:d.request(Operation.MICROPHONE,address=3,length=8,payload=struct.pack('<II',1,2)))
  rejected('overflow read',lambda:d.request(Operation.MICROPHONE,address=3,length=8,payload=struct.pack('<II',0xfffffffe,32)))
  rejected('malformed WAV path',lambda:d.request(Operation.MICROPHONE,address=4,length=7,payload=b'x\0tail\0'))
  audio.mute(False);audio.volume(60)
  for frequency in [440,660]:
   audio.tone(frequency,1000,16000);mic.start(32000,16000);i=mic.wait();data=mic.read(0,32000);done=audio.wait()
   peak,magnitude=spectrum(data,16000,frequency)
   assert abs(peak-frequency)<=2 and magnitude>5 and not done['timeouts']
   report['duplex'].append({'frequency':frequency,'captured_peak_Hz':peak,'amplitude':magnitude,'capture':i,'audio':done})
   print('duplex',frequency,peak,round(magnitude,1),flush=True)
  try:sd.mkdir('0:/SDKAudio')
  except ApxError as e:
   if 'FR_EXIST' not in str(e):raise
  path='0:/SDKAudio/microphone-'+info['build_id']+'.wav'
  mic.save(path);out=io.BytesIO();sd.download(path,out);wav_data=out.getvalue()
  with wave.open(io.BytesIO(wav_data),'rb') as w:
   assert (w.getnchannels(),w.getsampwidth(),w.getframerate())==(1,2,16000)
   assert w.readframes(w.getnframes())==data
  rejected('existing WAV protected',lambda:mic.save(path))
  report['WAV']={'path':path,'sha256':hashlib.sha256(wav_data).hexdigest(),'bytes':len(wav_data)}
  a.output.with_suffix('.wav').write_bytes(wav_data)
  audio.mute(True);mic.play()
  rejected('capture buffer owned by speaker',lambda:mic.start())
  audio.wait();audio.stop()
  start=time.monotonic();jobs=previews=sd_bytes=0
  while time.monotonic()-start<a.seconds:
   audio.tone(660,500,16000);mic.start(32000,16000)
   jpeg.preview('0:/SDKJPEG/BASELINE/jpeg-test-2048x1536.jpg');jpeg.wait();previews+=1
   assert sd.read(path,0,4096)==wav_data[:4096];sd_bytes+=4096
   gpio.info();system.info();d.info();i=mic.wait();audio.wait();jobs+=1
   assert i['guard_ok'] and not i['timeouts']
  report['sustained']={'seconds':time.monotonic()-start,'capture_jobs':jobs,'speaker_jobs':jobs,'JPEG_3m_previews':previews,'verified_SD_bytes':sd_bytes}
  assert int.from_bytes(d.read(0x884c,4,space=Space.SPR,width=4),'little')==clock
  report.update(clock_unchanged=True,microphone=mic.info(),audio=audio.info(),sd=sd.info(),gpio=gpio.info(),system=system.info(),stack_bottom_512_zero=not any(d.read(0x02200000,512)))
  assert report['stack_bottom_512_zero'] and not report['microphone']['timeouts'] and not report['audio']['timeouts'] and not report['sd']['errors'] and not report['gpio']['adc_timeouts']
  jpeg.preview('0:/SDKJPEG/BASELINE/jpeg-test-220x176.jpg');report['native_JPEG']=jpeg.wait()
  mic.stop();audio.mute(False);audio.volume(60);audio.stop();report['status']='passed';save()
except BaseException as e:report.update(status='failed',error=str(e));save();raise
print(json.dumps({'status':report['status'],'sustained':report['sustained']},indent=2))
