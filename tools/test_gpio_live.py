#!/usr/bin/env python3
"""Stress board IO alongside read-only SD and USB; never writes SPI/card."""
import argparse,hashlib,json,time,random,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.debug import USBTransport,DebugClient,Space,Operation
from apxtool.gpio import GPIOClient
from apxtool.storage import StorageClient
from apxtool.errors import DebugCommandError
p=argparse.ArgumentParser();p.add_argument('--port',default='7-1');p.add_argument('--seconds',type=float,default=60)
p.add_argument('--output',type=Path,required=True);args=p.parse_args()
if args.output.exists():p.error('output must be new')
research=Path(__file__).resolve().parents[1]/'research';report={'status':'running'}
try:
 with USBTransport(port=args.port) as transport:
  client=DebugClient(transport);gpio=GPIOClient(client);sd=StorageClient(client)
  report['sdk']=client.info();report['gpio_initial']=gpio.info();gpio.events()
  report['volume']=sd.volume();report['files']=[]
  for entry in json.loads((research/'sd-stock-files.json').read_text()):
   digest=hashlib.sha256();offset=0
   while True:
    data=sd.read('0:/'+entry['path'],offset);digest.update(data);offset+=len(data)
    gpio.info();gpio.events()
    if len(data)<4096:break
   assert offset==entry['bytes'] and digest.hexdigest()==entry['sha256']
   report['files'].append({**entry,'matches_stock':True})
  for command,payload in [(0,b'x'),(1,b''),(1,(2).to_bytes(4,'little')),(2,b'x'),(99,b'')]:
   try:client.request(Operation.GPIO,address=command,length=len(payload),payload=payload)
   except DebugCommandError:pass
   else:raise AssertionError('invalid GPIO request accepted')
  snapshots=json.loads((research/'sd-stock-baseline.json').read_text())['ranges']
  baselines=[(s,(research/s['file']).read_bytes()) for s in snapshots]
  start=time.monotonic();rng=random.Random(5295);reads=bytes_read=0;led=None;events=[]
  while time.monotonic()-start<args.seconds:
   wanted=int(time.monotonic()-start)%2
   if wanted!=led:gpio.led(wanted);led=wanted
   status=gpio.info();assert status['led']==led and not status['adc_timeouts']
   events.extend(gpio.events())
   snapshot,data=rng.choice(baselines);count=rng.choice((1,2,7,8));offset=rng.randrange(snapshot['count']-count+1)
   assert sd.sectors(snapshot['sector']+offset,count)==data[offset*512:(offset+count)*512]
   assert client.info()['build_id']==report['sdk']['build_id'];reads+=1;bytes_read+=512*count
  gpio.led(0);report['gpio_final']=gpio.info();report['sd_final']=sd.info()
  assert not report['sd_final']['errors'] and not report['gpio_final']['adc_timeouts']
  report['stress']={'seconds':time.monotonic()-start,'reads':reads,'verified_bytes':bytes_read,'events':events}
  report['logs']=client.logs().decode(errors='replace');report['status']='passed'
except BaseException as exc:report.update(status='failed',error=str(exc));raise
finally:args.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
