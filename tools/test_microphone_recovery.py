#!/usr/bin/env python3
"""Same-image BootROM recovery with both speaker and microphone DMA active."""
import argparse,json,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.debug import USBTransport,DebugClient
from apxtool.audio import AudioClient
from apxtool.microphone import MicrophoneClient
from apxtool.jpeg import JPEGClient
from apxtool.sdk import launch
from apxtool.debug_cli import wait_bootrom
p=argparse.ArgumentParser();p.add_argument('--port',required=True);p.add_argument('--manifest',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);a=p.parse_args()
if a.output.exists():p.error('output must be new')
manifest=json.loads(a.manifest.read_text());report={'status':'running','cycles':[]}
try:
 for n in range(3):
  with USBTransport(port=a.port) as t:
   d=DebugClient(t);info=d.info();assert info['build_id']==manifest['build_id']
   audio=AudioClient(d);mic=MicrophoneClient(d);audio.stop();mic.stop();audio.mute(True)
   audio.tone(440,2000,8000);mic.start(64000,8000)
   before={'audio':audio.info(),'microphone':mic.info()};assert before['audio']['state']==before['microphone']['state']==2
   d.bootrom()
  path=wait_bootrom(a.port,owned_ram=info['owned_ram']);t,handshake=launch(path,a.manifest,reclaim_jpeg_ram=True)
  with t:
   d=DebugClient(t);audio=AudioClient(d);mic=MicrophoneClient(d)
   assert audio.info()['state']==mic.info()['state']==1
   audio.mute(True);audio.tone(660,1000);mic.start(32000,16000)
   capture=mic.wait();done=audio.wait();assert capture['guard_ok'] and not capture['timeouts'] and not done['timeouts']
   jpeg=JPEGClient(d);end=time.monotonic()+15
   while jpeg.info()['job'] not in (5,6):
    if time.monotonic()>end:raise RuntimeError('initial JPEG did not complete')
    time.sleep(.02)
   final=jpeg.wait();mic.stop();audio.mute(False);audio.volume(60);audio.stop()
  report['cycles'].append({'cycle':n+1,'active_before_handoff':before,'loader_verified':path,'handshake':handshake,'capture_after':capture,'audio_after':done,'JPEG_after':final})
  a.output.write_text(json.dumps(report,indent=2)+'\n');print('duplex recovery',n+1,'passed',flush=True)
 report['status']='passed'
except BaseException as e:report.update(status='failed',error=str(e));raise
finally:a.output.write_text(json.dumps(report,indent=2)+'\n')
