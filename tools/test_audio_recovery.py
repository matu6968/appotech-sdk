#!/usr/bin/env python3
"""Verify BootROM recovery while speaker DMA is active; relaunch same image."""
import argparse,json,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.debug import USBTransport,DebugClient
from apxtool.audio import AudioClient
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
   d=DebugClient(t);i=d.info();assert i['build_id']==manifest['build_id']
   audio=AudioClient(d);audio.stop();audio.mute(True);audio.tone(440,2000,8000)
   before=audio.info();assert before['state']==2;d.bootrom()
  path=wait_bootrom(a.port,owned_ram=i['owned_ram']);t,handshake=launch(path,a.manifest,reclaim_jpeg_ram=True)
  with t:
   d=DebugClient(t);audio=AudioClient(d);assert audio.info()['state']==1
   audio.mute(True);audio.tone(660,1000);done=audio.wait();assert not done['timeouts']
   jpeg=JPEGClient(d);end=time.monotonic()+15
   while jpeg.info()['job'] not in (5,6):
    if time.monotonic()>end:raise RuntimeError('initial JPEG did not complete')
    time.sleep(.02)
   final=jpeg.wait();audio.mute(False);audio.volume(60);audio.stop()
  report['cycles'].append({'cycle':n+1,'active_audio_before_handoff':before,'loader_verified':path,'handshake':handshake,'audio_after':done,'JPEG_after':final})
  a.output.write_text(json.dumps(report,indent=2)+'\n');print('active audio recovery',n+1,'passed',flush=True)
 report['status']='passed'
except BaseException as e:report.update(status='failed',error=str(e));raise
finally:a.output.write_text(json.dumps(report,indent=2)+'\n')
