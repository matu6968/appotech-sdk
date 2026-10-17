#!/usr/bin/env python3
"""Same-image JPEG/BootROM cycles; excludes layout-changing SPI callbacks."""
import argparse,json,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.debug import DebugClient,USBTransport
from apxtool.jpeg import JPEGClient
from apxtool.sdk import launch
from apxtool.debug_cli import wait_bootrom
from apxtool.transport import LinuxSCSITransport,inquiry
from apxtool.ram import read_ram

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True)
p.add_argument('--manifest',type=Path,required=True)
p.add_argument('--output',type=Path,required=True);p.add_argument('--cycles',type=int,default=3)
a=p.parse_args()
if a.output.exists():p.error('output must be new')
if not 1<=a.cycles<=10:p.error('cycles must be 1..10')
manifest=json.loads(a.manifest.read_text());build=manifest['build_id']
report={'status':'running','cycles':[],'build_id':build}
try:
    for cycle in range(a.cycles):
        start=time.monotonic()
        with USBTransport(port=a.port) as t:
            d=DebugClient(t);i=d.info();assert i['build_id']==build
            j=JPEGClient(d)
            if cycle==0:path='0:/SDKJPEG/BASELINE/jpeg-test-2048x1536.jpg'
            elif cycle==1:path=f'0:/SDKJPEG/ENCODER/encode-2048x1536-{build}.jpg'
            else:path=f'0:/SDKJPEG/BASELINE/jpeg-test-422-{build}.jpg'
            j.preview(path);result=j.wait();d.bootrom()
        device_path=wait_bootrom(a.port,owned_ram=i['owned_ram'])
        with LinuxSCSITransport(device_path) as device:
            identity=inquiry(device);assert identity.is_loader
            ram=read_ram(device,0x02000000,16)
        transport,info=launch(device_path,a.manifest,reclaim_jpeg_ram=True)
        with transport:
            j=JPEGClient(DebugClient(transport))
            # USB handshake can precede the demo's asynchronous initial preview.
            deadline=time.monotonic()+15
            while j.info()['job'] not in (5,6):
                if time.monotonic()>deadline:raise RuntimeError('initial preview did not finish')
                time.sleep(.02)
            final=j.wait()
        report['cycles'].append({'cycle':cycle+1,'preview':result,'loader_path':device_path,
            'loader_ram_hex':ram.hex(),'handshake':info,'final_preview':final,'seconds':time.monotonic()-start})
        a.output.write_text(json.dumps(report,indent=2)+'\n');print('cycle',cycle+1,'passed',flush=True)
    report['status']='passed'
except BaseException as e:report.update(status='failed',error=str(e));raise
finally:a.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'status':report['status'],'cycles':len(report['cycles'])},indent=2))
