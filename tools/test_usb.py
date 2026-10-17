#!/usr/bin/env python3
"""Live APXD acceptance checks. Only uses preflighted zero scratch RAM."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'apxtool/src'))
from apxtool.debug import DebugClient,USBTransport,Frame,HEADER,Operation,Space
from apxtool.errors import TransferError

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--port',default='7-1')
    parser.add_argument('--seconds',type=float,default=600)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.exists():parser.error('output must be new')
    report={'status':'running','port':args.port,'checks':[]}
    transport=USBTransport(port=args.port);client=DebugClient(transport)
    scratch=0x021a0000;size=8192;original=None
    try:
        report['info']=client.info()
        report['linux_speed_mbps']=(Path('/sys/bus/usb/devices')/args.port/'speed').read_text().strip()
        report['logs']=client.logs().decode(errors='replace')
        dev=transport.device
        report['device_descriptor']=bytes(dev.ctrl_transfer(0x80,6,0x100,0,18)).hex()
        report['qualifier']=bytes(dev.ctrl_transfer(0x80,6,0x600,0,10)).hex()
        report['other_speed']=bytes(dev.ctrl_transfer(0x80,6,0x700,0,32)).hex()
        assert len(bytes.fromhex(report['qualifier']))==10
        other=bytes.fromhex(report['other_speed'])
        assert other[1]==7 and int.from_bytes(other[22:24],'little')==(64 if report['info']['usb_speed']=='high' else 512)
        report['checks'].append('descriptors and opposite-speed packet sizes')
        original=client.read(scratch,size)
        if any(original):raise RuntimeError('scratch RAM is occupied')
        for length in (0,1,2,4,63,64,65,487,488,489,511,512,513,4071,4072,4073,4095,4096,4097,8192):
            data=bytes((i*37+length)&255 for i in range(length))
            client.write(scratch,data)
            assert client.read(scratch,length)==data,f'boundary {length}'
        for space in (Space.MEMORY,Space.MMIO):
            for width in (1,2,4):
                data=bytes(range(64));client.write(scratch,data,space=space,width=width)
                assert client.read(scratch,64,space=space,width=width)==data
        report['checks'].append('read/write widths, MMIO space and 64/512/4096 boundaries')
        report['timer_spr']=client.read(0x9804,4,space=Space.SPR,width=4).hex()
        client.write(0x880c,(0x5001d).to_bytes(4,'little'),space=Space.SPR,width=4)
        def status(frame,wanted):
            response=Frame.decode(transport.exchange(frame.encode()))
            assert response.status==wanted,(response.status,wanted)
        status(Frame(Operation.READ,width=4,address=scratch+1,length=4),2)
        status(Frame(Operation.READ,address=0xfffffff0,length=32),2)
        status(Frame(Operation.WRITE,address=0x02180000,length=1,payload=b'\0'),4)
        status(Frame(Operation.READ,space=3,address=scratch,length=1),3)
        status(Frame(Operation.INFO,status=1),1)
        status(Frame(Operation.INFO,address=1),1)
        invalid_version=bytearray(Frame(Operation.INFO).encode());invalid_version[4]=2
        reply=transport.exchange(bytes(invalid_version));assert len(reply)==24 and int.from_bytes(reply[20:24],'little')==1
        for malformed in (b'BAD!'+Frame(Operation.INFO).encode()[4:],
                          HEADER.pack(b'APXD',1,1,0,1,0,scratch,4097,0),b'APXD\x01'):
            old_timeout=transport.timeout_ms;transport.timeout_ms=1000
            try:
                try:transport.exchange(malformed)
                except TransferError as exc:
                    if 'timed out' not in str(exc).lower() and '110' not in str(exc):raise
                else:raise RuntimeError('malformed frame was accepted')
            finally:transport.timeout_ms=old_timeout
            client.info()
        reserved=0x0219fff0;before=client.read(reserved,16)
        try:
            client.write(reserved,b'RAW protection!!',raw=True)
            assert client.read(reserved,16)==b'RAW protection!!'
        finally:client.write(reserved,before,raw=True)
        report['checks'].append('malformed frames/ranges, parser timeout recovery, protected RAM, explicit raw override and SPR')
        for ep in (1,0x81):
            dev.ctrl_transfer(0x02,3,0,ep,b'')
            assert bytes(dev.ctrl_transfer(0x82,0,0,ep,2))==b'\x01\x00'
            dev.clear_halt(ep)
            assert bytes(dev.ctrl_transfer(0x82,0,0,ep,2))==b'\0\0'
            client.info()
        import usb.core
        try:dev.ctrl_transfer(0x80,0xff,0,0,1,timeout=1000)
        except usb.core.USBError as exc:
            if exc.errno!=32:raise
        else:raise RuntimeError('unsupported control request did not stall')
        client.info();report['checks'].append('endpoint halt recovery and EP0 stall recovery')
        start=time.monotonic();next_update=start+30;cycles=0;total=0
        while time.monotonic()-start<args.seconds:
            length=(64,512,4096,4097)[cycles%4]
            data=bytes((i*17+cycles)&255 for i in range(length))
            client.write(scratch,data);assert client.read(scratch,length)==data
            cycles+=1;total+=length*2
            if time.monotonic()>=next_update:
                print(json.dumps({'seconds':round(time.monotonic()-start,1),'cycles':cycles,'bytes':total}),flush=True)
                next_update+=30
        report.update(status='passed',elapsed_seconds=time.monotonic()-start,cycles=cycles,bytes=total)
        report['checks'].append('sustained verified bulk transfers')
    except BaseException as exc:
        report.update(status='failed',error=str(exc));raise
    finally:
        try:
            if original is not None:
                client.write(scratch,original)
                report['scratch_restored']=client.read(scratch,size)==original
        finally:
            transport.close();args.output.parent.mkdir(parents=True,exist_ok=True)
            args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)

if __name__=='__main__':main()
