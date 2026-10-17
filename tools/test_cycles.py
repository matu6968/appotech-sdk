#!/usr/bin/env python3
"""Ten complete RAM launch/APXD/recovery cycles on one physical camera port."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import subprocess
import io
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.sdk import launch,scsi_port
from apxtool.debug import DebugClient,Space
from apxtool.debug_cli import wait_bootrom
from apxtool.transport import iter_sg_paths

def unmount_camera_partitions(path):
    """Close desktop automounts before SDK takeover; only this SCSI device."""
    root=Path('/sys/class/scsi_generic')/Path(path).name/'device/block'
    mounted={line.split()[2] for line in Path('/proc/self/mountinfo').read_text().splitlines()}
    for disk in root.iterdir():
        for node in [disk,*disk.iterdir()]:
            if not (node/'dev').exists() or (node/'dev').read_text().strip() not in mounted:continue
            subprocess.run(['udisksctl','unmount','-b','/dev/'+node.name],check=True,stdout=subprocess.DEVNULL)

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--port',default='7-1')
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--count',type=int,default=10)
    parser.add_argument('--display-elf',type=Path,help='wait for display initialization and test two redraws in each cycle')
    parser.add_argument('--sd',action='store_true',help='mount/list/read SD through the independent driver before recovery')
    parser.add_argument('--gpio',action='store_true',help='verify board input sampling and both status LED states')
    parser.add_argument('--sd-write',action='store_true',help='write SDKCYCLE.BIN and verify it after remount/recovery; implies --sd')
    parser.add_argument('--system',action='store_true',help='verify watchdog auto/manual/explicit-feed policy before recovery')
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    if args.output.exists():parser.error('output must be new')
    if args.sd_write:args.sd=True
    report={'status':'running','port':args.port,'manifest_sha256':hashlib.sha256(args.manifest.read_bytes()).hexdigest(),'cycles':[]}
    gpio_display_symbols=None
    if args.gpio:
        from elftools.elf.elffile import ELFFile
        elf_path=args.manifest.with_name(args.manifest.name.removesuffix('.manifest.json')+'.elf')
        with elf_path.open('rb') as source:
            symbols=ELFFile(source).get_section_by_name('.symtab')
            gpio_display_symbols={s.name:s['st_value'] for s in symbols.iter_symbols()
                                  if s.name in ('state','timeout_count','pixel_count')}
        if set(gpio_display_symbols)!=set(('state','timeout_count','pixel_count')):
            raise RuntimeError('GPIO demo display symbols missing')
    try:
        for cycle in range(args.count):
            paths=[path for path in iter_sg_paths() if scsi_port(path)==args.port]
            if len(paths)!=1:raise RuntimeError('camera SCSI interface is unavailable')
            if args.sd:unmount_camera_partitions(paths[0])
            start=time.monotonic();transport,info=launch(paths[0],args.manifest)
            with transport:
                client=DebugClient(transport);scratch=0x021a0000
                system_result=None
                if args.system:
                    from apxtool.system import SystemClient
                    system=SystemClient(client);system_result={'default':system.info()}
                    assert system_result['default']['watchdog']['automatic'] and system_result['default']['watchdog']['selector']==5
                    system.watchdog_start(6,automatic=False);manual=system.info()['watchdog'];client.info()
                    assert system.info()['watchdog']['feeds']==manual['feeds']
                    system.watchdog_feed();fed=system.info()['watchdog'];assert fed['feeds']==manual['feeds']+1
                    system.watchdog_start(5,automatic=True)
                    system_result.update(manual=manual,explicit_feed=fed,restored=system.info())
                storage=None
                gpio_result=None
                if args.gpio:
                    from apxtool.gpio import GPIOClient
                    gpio=GPIOClient(client);gpio_result={'initial':gpio.info(),'states':[]}
                    for enabled in (1,0):
                        gpio.led(enabled)
                        high=int.from_bytes(client.read(0x81c4,4,space=Space.SPR,width=4),'little')
                        low=int.from_bytes(client.read(0x8204,4,space=Space.SPR,width=4),'little')
                        assert bool(high&0x20)==bool(enabled) and bool(low&0x20)!=bool(enabled)
                        status=gpio.info();assert status['led']==enabled and not status['adc_timeouts']
                        gpio_result['states'].append(status)
                    gpio_result['events']=gpio.events()
                if args.sd:
                    from apxtool.storage import StorageClient
                    sd=StorageClient(client)
                    volume=sd.volume();root=list(sd.list())
                    sector=sd.sectors(0,8)
                    assert sector[510:512]==b'\x55\xaa'
                    storage={'volume':volume,'root':root,'sector0_7_sha256':hashlib.sha256(sector).hexdigest(),'info':sd.info()}
                    assert storage['info']['initialized'] and not storage['info']['errors']
                    if args.sd_write:
                        previous=None
                        if cycle:
                            previous=bytes((i*17+cycle-1)&255 for i in range(4097))
                            actual=io.BytesIO();sd.download('0:/SDKCYCLE.BIN',actual);assert actual.getvalue()==previous
                        data=bytes((i*17+cycle)&255 for i in range(4097))
                        assert sd.upload('0:/SDKCYCLE.BIN',io.BytesIO(data),overwrite=True)==len(data)
                        sd.sync();sd.unmount();sd.volume()
                        actual=io.BytesIO();sd.download('0:/SDKCYCLE.BIN',actual);assert actual.getvalue()==data
                        storage['write']={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
                                          'verified_after_remount':True,'previous_cycle_survived_bootrom':previous is not None}
                        storage['info_after_write']=sd.info();assert not storage['info_after_write']['errors']
                display=None
                if args.display_elf:
                    from display_demo import symbols,wait_ready,show
                    display_manifest=json.loads(args.display_elf.with_suffix('.manifest.json').read_text())
                    if display_manifest['build_id']!=info['build_id']:raise RuntimeError('display ELF does not match SDK')
                    address=symbols(args.display_elf)['apx_display_demo_control']
                    wait_ready(client,address)
                    display=[show(client,address,8),show(client,address,0)]
                if args.gpio:
                    until=time.monotonic()+10
                    while time.monotonic()<until:
                        fields={name:int.from_bytes(client.read(address,4,width=4),'little')
                                for name,address in gpio_display_symbols.items()}
                        if fields['state']==4 or fields['timeout_count']:raise RuntimeError('GPIO display failed')
                        if fields['pixel_count']>=70400 and fields['state']==2:break
                        time.sleep(.01)
                    else:raise RuntimeError('GPIO display redraw did not finish')
                    gpio_result['display']=fields
                    gpio_result['after_sd']=gpio.info();assert not gpio_result['after_sd']['adc_timeouts']
                    assert not any(client.read(0x02200000,512))
                    gpio_result['stack_bottom_512_zero']=True
                original=client.read(scratch,4096)
                if any(original):raise RuntimeError('cycle scratch RAM is occupied')
                try:
                    data=bytes((i*17+cycle)&255 for i in range(4096))
                    client.write(scratch,data,width=4);assert client.read(scratch,4096,width=4)==data
                    timer=client.read(0x9804,4,space=Space.SPR,width=4).hex()
                    logs=client.logs().decode(errors='replace')
                finally:
                    client.write(scratch,original);assert client.read(scratch,4096)==original
                client.bootrom() # Failure to receive the ACK is a failed cycle.
            loader=wait_bootrom(args.port,owned_ram=info['owned_ram'])
            result={'cycle':cycle+1,'info':info,'timer':timer,'logs':logs,'acknowledged':True,
                    'bootrom_inquiry_and_ram_read':True,'loader_path':loader,'elapsed':time.monotonic()-start}
            if display is not None:result['display']=display
            if storage is not None:result['storage']=storage
            if gpio_result is not None:result['gpio']=gpio_result
            if system_result is not None:result['system']=system_result
            report['cycles'].append(result);print(json.dumps(result),flush=True)
            args.output.write_text(json.dumps(report,indent=2)+'\n')
        report['status']='passed'
    except BaseException as exc:
        report.update(status='failed',error=str(exc));raise
    finally:args.output.write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
