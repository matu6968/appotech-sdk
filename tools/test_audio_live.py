#!/usr/bin/env python3
"""Integrated speaker acceptance; creates only dedicated SDKAudio test files."""
import argparse,hashlib,io,json,math,struct,sys,time,wave
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.debug import USBTransport,DebugClient,Operation
from apxtool.audio import AudioClient
from apxtool.jpeg import JPEGClient
from apxtool.storage import StorageClient
from apxtool.gpio import GPIOClient
from apxtool.system import SystemClient
from apxtool.errors import ApxError,DebugCommandError
from elftools.elf.elffile import ELFFile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True)
p.add_argument('--elf',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--seconds',type=float,default=60);a=p.parse_args()
if a.output.exists():p.error('output must be new')
manifest=json.loads(a.elf.with_suffix('.manifest.json').read_text())
with a.elf.open('rb') as f:symbols={s.name:s['st_value'] for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()}
report={'status':'running','rates':[],'PCM':[],'negative':[]}
def save():a.output.write_text(json.dumps(report,indent=2)+'\n')
try:
 with USBTransport(port=a.port) as t:
  d=DebugClient(t);audio=AudioClient(d);sd=StorageClient(d);gpio=GPIOClient(d);system=SystemClient(d);jpeg=JPEGClient(d)
  report['sdk']=d.info();assert report['sdk']['build_id']==manifest['build_id'];build=manifest['build_id']
  audio.stop();audio.volume(60);audio.mute(True)
  for rate in [8000,16000,32000]:
   for trial in range(3):
    audio.tone(440,1000,rate);i=audio.wait();measured=(i['bytes']//2-7)*32768/i['ticks']
    assert abs(measured/rate-1)<.01 and not i['error'] and not i['timeouts']
    report['rates'].append({'requested':rate,'trial':trial+1,'counter_rate':measured,'info':i});print(rate,round(measured,2),'samples/s',flush=True)
  def rejected(case,fn):
   before=audio.info()['sequence']
   try:fn()
   except ApxError as e:report['negative'].append({'case':case,'error':str(e)})
   else:raise AssertionError('accepted '+case)
   assert audio.info()['sequence']==before
  for n in [2,62,64,126,1024,4092,4094,4096,8190,65504]:
   pcm=struct.pack('<'+str(n//2)+'h',*[int(7000*math.sin(2*math.pi*440*x/16000)) for x in range(n//2)])
   audio.pcm(pcm);i=audio.wait();actual=d.read(i['buffer'],i['bytes'])
   assert actual[:n]==pcm and not any(actual[n:]) and i['bytes']%32==0
   report['PCM'].append({'bytes':n,'padded':i['bytes'],'sha256':hashlib.sha256(pcm).hexdigest(),'ticks':i['ticks']})
  audio.tone(440,1500);rejected('busy tone',lambda:audio.tone(660,500))
  rejected('busy PCM upload',lambda:d.request(Operation.AUDIO,address=6,length=6,payload=bytes(6)))
  audio.stop();assert audio.info()['state']==1
  for command,payload in [(1,b'x'),(4,struct.pack('<I',101)),(5,struct.pack('<I',2)),(6,struct.pack('<I',2)+bytes(2)),(7,struct.pack('<I',44100)),(2,b'bad\0tail\0')]:
   rejected('invalid command '+str(command),lambda command=command,payload=payload:d.request(Operation.AUDIO,address=command,length=len(payload),payload=payload))
  for percent in [0,25,60,100]:audio.volume(percent);assert audio.info()['volume']==percent
  audio.volume(60);audio.mute(False);audio.tone(440,1000);time.sleep(.15)
  audio.mute(True);assert audio.info()['muted']==1;time.sleep(.15);audio.mute(False);assert audio.info()['muted']==0;audio.wait()
  report['mute_unmute_during_playback']=True;report['stop_and_restart']=True
  try:sd.mkdir('0:/SDKAudio')
  except ApxError as e:
   if 'FR_EXIST' not in str(e):raise
  pcm=struct.pack('<16000h',*[int(9000*math.sin(2*math.pi*660*x/16000)) for x in range(16000)])
  wav_data=io.BytesIO()
  with wave.open(wav_data,'wb') as out:out.setnchannels(1);out.setsampwidth(2);out.setframerate(16000);out.writeframes(pcm)
  good=wav_data.getvalue();path=f'0:/SDKAudio/tone-{build}.wav';sd.upload(path,io.BytesIO(good));audio.wav(path);i=audio.wait()
  assert d.read(i['buffer'],len(pcm))==pcm;report['WAV']={'path':path,'sha256':hashlib.sha256(good).hexdigest(),'info':i}
  bad=bytearray(good);bad[22]=2;badpath=f'0:/SDKAudio/stereo-invalid-{build}.wav';sd.upload(badpath,io.BytesIO(bad));rejected('stereo WAV',lambda:audio.wav(badpath))
  truncated=f'0:/SDKAudio/truncated-{build}.wav';sd.upload(truncated,io.BytesIO(good[:-2]));rejected('truncated WAV',lambda:audio.wav(truncated))
  rejected('missing WAV',lambda:audio.wav('0:/SDKAudio/no-such-file.wav'))
  audio.mute(True);start=time.monotonic();jobs=previews=reads=0
  while time.monotonic()-start<a.seconds:
   audio.tone(660,500,16000)
   jpeg.preview('0:/SDKJPEG/BASELINE/jpeg-test-2048x1536.jpg');jpeg.wait();previews+=1
   assert sd.read(path,0,4096)==good[:4096];reads+=4096
   gpio.info();d.info();system.info();audio.wait();jobs+=1
  report['sustained']={'seconds':time.monotonic()-start,'audio_jobs':jobs,'JPEG_3m_previews':previews,'verified_sd_bytes':reads}
  assert not any(d.read(0x02200000,512));report['stack_bottom_512_zero']=True
  report.update(audio=audio.info(),sd=sd.info(),gpio=gpio.info(),system=system.info())
  report['display']={name:int.from_bytes(d.read(symbols[name],4,width=4),'little') for name in ['timeout_count','pixel_count']}
  assert not report['audio']['timeouts'] and not report['sd']['errors'] and not report['gpio']['adc_timeouts'] and not report['display']['timeout_count']
  jpeg.preview('0:/SDKJPEG/BASELINE/jpeg-test-220x176.jpg');report['final_JPEG']=jpeg.wait()
  audio.mute(False);audio.volume(60);audio.stop();report['final_audio']=audio.info();report['status']='passed';save()
except BaseException as e:report.update(status='failed',error=str(e));save();raise
print(json.dumps({'status':report['status'],'sustained':report['sustained']},indent=2))
