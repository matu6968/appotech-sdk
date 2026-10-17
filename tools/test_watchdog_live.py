#!/usr/bin/env python3
"""Test automatic/manual watchdog while USB, display, GPIO and SD stay active.
Writes only the dedicated SDKWDT.BIN test file on the authorized disposable card.
"""
import argparse,hashlib,io,json,struct,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.debug import USBTransport,DebugClient,Operation
from apxtool.system import SystemClient
from apxtool.storage import StorageClient
from apxtool.gpio import GPIOClient
from apxtool.errors import DebugCommandError
from elftools.elf.elffile import ELFFile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',default='7-1')
p.add_argument('--elf',type=Path,required=True);p.add_argument('--seconds',type=float,default=60)
p.add_argument('--output',type=Path,required=True);a=p.parse_args()
if a.output.exists():p.error('output must be new')
if a.seconds<4:p.error('test needs at least four seconds')
manifest=json.loads(a.elf.with_suffix('.manifest.json').read_text())
with a.elf.open('rb') as f:
 symbols={s.name:s['st_value'] for s in ELFFile(f).get_section_by_name('.symtab').iter_symbols()}
report={'status':'running','modes':[]}
try:
 with USBTransport(port=a.port) as t:
  c=DebugClient(t);system=SystemClient(c);sd=StorageClient(c);gpio=GPIOClient(c)
  report['sdk']=c.info();assert report['sdk']['build_id']==manifest['build_id'];sd.volume()
  report['default']=system.info();system.watchdog_stop();time.sleep(2.5)
  stopped=system.info();assert not stopped['watchdog']['enabled'] and not stopped['watchdog']['automatic']
  report['disabled_survives_default_timeout']=True
  system.watchdog_start(6,automatic=False);before=system.info()['watchdog']
  for command,payload in [(1,struct.pack('<II',8,1)),(1,struct.pack('<II',5,2)),(1,bytes(4)),(2,b'x'),(3,b'x'),(4,b'x'),(5,b'x')]:
   try:c.request(Operation.SYSTEM,address=command,length=len(payload),payload=payload)
   except DebugCommandError:pass
   else:raise AssertionError('invalid system request accepted')
  after=system.info()['watchdog'];assert after['selector']==6 and after['feeds']==before['feeds']
  report['malformed_requests_leave_watchdog_unchanged']=True
  content=bytearray((i*29+0x95)&255 for i in range(65537));system.watchdog_start(5,automatic=True)
  sd.upload('0:/SDKWDT.BIN',io.BytesIO(content),overwrite=True)
  for automatic,selector in [(True,5),(False,6)]:
   system.watchdog_start(selector,automatic=automatic);start=time.monotonic();requests=0;written=0
   previous=system.info()['watchdog']['feeds']
   while time.monotonic()-start<a.seconds/2:
    if not automatic:system.watchdog_feed();previous=system.info()['watchdog']['feeds']
    offset=(requests*509)%(len(content)-2048);data=bytes((i*17+requests)&255 for i in range(2048))
    sd.write('0:/SDKWDT.BIN',data,offset);assert sd.read('0:/SDKWDT.BIN',offset,len(data))==data
    content[offset:offset+len(data)]=data;gpio.info();gpio.events();c.info()
    status=system.info()['watchdog'];assert status['enabled'] and status['automatic']==int(automatic)
    if not automatic:assert status['feeds']==previous
    requests+=1;written+=len(data)
   report['modes'].append({'automatic':automatic,'selector':selector,'seconds':time.monotonic()-start,
                          'requests':requests,'verified_bytes':written,'final_watchdog':system.info()['watchdog']})
   system.watchdog_start(5,automatic=True)
  sd.sync();sd.unmount();sd.volume();actual=io.BytesIO();sd.download('0:/SDKWDT.BIN',actual)
  assert actual.getvalue()==content
  display={name:int.from_bytes(c.read(symbols[name],4,width=4),'little') for name in ('state','timeout_count','pixel_count')}
  assert display['state']==2 and not display['timeout_count'] and display['pixel_count']>=70400
  assert not any(c.read(0x02200000,512));assert not sd.info()['errors'] and not gpio.info()['adc_timeouts']
  report.update(status='passed',display=display,sd=sd.info(),gpio=gpio.info(),
                stack_bottom_512_zero=True,file={'path':'SDKWDT.BIN','bytes':len(content),'sha256':hashlib.sha256(content).hexdigest()},
                logs=c.logs().decode(errors='replace'))
except BaseException as exc:report.update(status='failed',error=str(exc));raise
finally:a.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'status':report['status'],'modes':report['modes']},indent=2))
