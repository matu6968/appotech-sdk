#!/usr/bin/env python3
"""Control/validate the optional display-demo mailbox over the SDK debugger."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import time
from elftools.elf.elffile import ELFFile
from apxtool.debug import DebugClient, USBTransport

FIELDS=('magic','version','state','request_sequence','completed_sequence','pattern',
        'framebuffer','width','height','pixels_sent','timeouts','frames')
PATTERNS=('bars','gradient','checker','red','green','blue','white','black','rectangle','native')
CONTROL=struct.Struct('<12I')

def symbols(path):
    with path.open('rb') as f:
        elf=ELFFile(f)
        return {s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}

def status(client,address):
    result=dict(zip(FIELDS,CONTROL.unpack(client.read(address,CONTROL.size,width=4))))
    if result['magic']!=0x31505344 or result['version'] not in (1,2):
        raise RuntimeError('display-demo mailbox not initialized')
    if (result['width'],result['height'])!=(220,176):
        raise RuntimeError('unexpected panel dimensions')
    if result['version']==2:
        result.update(zip(('dma_enabled','dma_transfers','last_transfer_ticks'),struct.unpack('<3I',client.read(address+48,12,width=4))))
    return result

def wait_ready(client,address,sequence=None,timeout=15):
    deadline=time.monotonic()+timeout
    exchanges=0
    while time.monotonic()<deadline:
        result=status(client,address);exchanges+=1
        if result['state']==4 or result['timeouts']:
            raise RuntimeError(f'display bus failed: {result}')
        target=result['request_sequence'] if sequence is None else sequence
        if result['state']==2 and result['completed_sequence']==target:
            return result,exchanges
        time.sleep(.002)
    raise RuntimeError(f'display did not finish: {result}')

def show(client,address,pattern):
    before,_=wait_ready(client,address)
    sequence=(before['request_sequence']+1)&0xffffffff
    client.write(address+20,struct.pack('<I',pattern),width=4,raw=True)
    client.write(address+12,struct.pack('<I',sequence),width=4,raw=True)
    start=time.monotonic()
    after,exchanges=wait_ready(client,address,sequence)
    pixels=64*80 if pattern==8 else 176*220
    if ((after['pixels_sent']-before['pixels_sent'])&0xffffffff)!=pixels:
        raise RuntimeError('incorrect pixel count')
    if after['frames']!=before['frames']+1:
        raise RuntimeError('frame completion counter mismatch')
    return {'pattern':PATTERNS[pattern],'seconds':time.monotonic()-start,
            'usb_status_exchanges':exchanges,'pixels':pixels,'status':after}

def nv21_from_rgb565(data,width=220,height=176,pitch=224):
    rgb=struct.unpack('<'+str(width*height)+'H',data)
    result=bytearray(pitch*height*3//2)
    result[pitch*height:]=bytes([128])*(pitch*height//2)
    def channels(pixel):
        r=pixel>>11;g=(pixel>>5)&63;b=pixel&31
        return (r<<3|r>>2,g<<2|g>>4,b<<3|b>>2)
    for y in range(height):
        for x in range(width):
            r,g,b=channels(rgb[y*width+x]);result[y*pitch+x]=(77*r+150*g+29*b)>>8
    for y in range(0,height,2):
        for x in range(0,width,2):
            colors=[channels(rgb[(y+dy)*width+x+dx]) for dy in (0,1) for dx in (0,1)]
            u=sum(-43*r-85*g+128*b for r,g,b in colors)
            v=sum(128*r-107*g-21*b for r,g,b in colors)
            offset=pitch*height+y//2*pitch+x
            result[offset]=max(0,min(255,128+(v>>10)))
            result[offset+1]=max(0,min(255,128+(u>>10)))
    return bytes(result)

def native_frame(client,address):
    show(client,address,0)
    ready,_=wait_ready(client,address)
    data=nv21_from_rgb565(client.read(ready['framebuffer'],77440,width=2))
    client.write(ready['framebuffer'],data,width=4,raw=True)
    entry=show(client,address,9)
    if client.read(ready['framebuffer'],len(data),width=4)!=data:
        raise RuntimeError('native DMA changed its source')
    entry['source_sha256']=hashlib.sha256(data).hexdigest()
    return entry

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf',type=Path,default=Path(__file__).resolve().parents[1]/'build/display-demo.elf')
    parser.add_argument('--port',default='7-1')
    parser.add_argument('--pattern',choices=PATTERNS)
    parser.add_argument('--test',action='store_true',help='exercise all patterns and verify framebuffer/USB, then restore bars')
    parser.add_argument('--stress-seconds',type=float,default=0,help='repeated full redraws while exchanging USB status requests')
    parser.add_argument('--stress-mode',choices=('rgb','native'),default='rgb')
    parser.add_argument('--mode',choices=('pio','dma'),help='select RGB565 transfer path')
    parser.add_argument('--native-test',action='store_true',help='upload a native NV21 frame to the idle example buffer and display it')
    parser.add_argument('--benchmark',action='store_true',help='compare PIO, converted RGB565 DMA and native NV21 DMA')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.stress_seconds<0:parser.error('stress duration must be nonnegative')
    address=symbols(args.elf)['apx_display_demo_control']
    manifest=json.loads(args.elf.with_suffix('.manifest.json').read_text())
    with USBTransport(port=args.port) as transport:
        client=DebugClient(transport);info=client.info()
        if info['build_id']!=manifest['build_id']:
            raise RuntimeError('active SDK does not match the selected ELF')
        initial,_=wait_ready(client,address)
        if args.mode:
            client.write(address+48,struct.pack('<I',args.mode=='dma'),width=4,raw=True)
        report={'build_id':info['build_id'],'usb_speed':info['usb_speed'],
                'control_address':address,'initial':initial,'patterns':[],
                'scope':'Driver completion, pixel counts, framebuffer readback and USB responsiveness; optical confirmation requires observing the panel.'}
        if args.pattern=='native' and not args.native_test:parser.error('native requires --native-test to prepare its source')
        if args.test:
            for pattern in range(9):
                entry=show(client,address,pattern)
                fb=client.read(entry['status']['framebuffer'],176*220*2,width=2)
                if 3<=pattern<=7:
                    color=(0xf800,0x07e0,0x001f,0xffff,0)[pattern-3]
                    if fb!=struct.pack('<H',color)*(176*220):
                        raise RuntimeError('solid pattern framebuffer readback failed')
                entry['framebuffer_sha256']=hashlib.sha256(fb).hexdigest()
                # Readback and INFO travel on the same independent USB path during display use.
                if client.info()['build_id']!=info['build_id']:raise RuntimeError('SDK changed during test')
                report['patterns'].append(entry)
                print(json.dumps(entry),flush=True)
            report['patterns'].append(show(client,address,0))
        elif args.pattern is not None:
            if args.pattern!='native':report['patterns'].append(show(client,address,PATTERNS.index(args.pattern)))
        if args.benchmark:
            benchmark={}
            for name,mode in (('pio',0),('rgb565_dma',1)):
                client.write(address+48,struct.pack('<I',mode),width=4,raw=True)
                benchmark[name]=[show(client,address,0) for _ in range(3)]
            benchmark['nv21_dma']=[native_frame(client,address) for _ in range(3)]
            report['benchmark']=benchmark
            show(client,address,0)
        elif args.native_test:report['patterns'].append(native_frame(client,address))
        if args.stress_seconds:
            if args.stress_mode=='native':
                native_frame(client,address)
                prepared,_=wait_ready(client,address)
                native_data=client.read(prepared['framebuffer'],224*176*3//2,width=4)
            start=time.monotonic();frames=exchanges=0;slowest=0
            while time.monotonic()-start<args.stress_seconds:
                entry=show(client,address,9 if args.stress_mode=='native' else 1+(frames&1))
                frames+=1;exchanges+=entry['usb_status_exchanges']
                slowest=max(slowest,entry['seconds'])
                if frames%25==0:print(f'stress: {frames} redraws, {time.monotonic()-start:.1f}s',flush=True)
            report['stress']={'seconds':time.monotonic()-start,'redraws':frames,
                              'mode':args.stress_mode,'usb_status_exchanges':exchanges,'slowest_redraw_seconds':slowest}
            if args.stress_mode=='native' and client.read(prepared['framebuffer'],len(native_data),width=4)!=native_data:
                raise RuntimeError('sustained native DMA changed its source')
            report['patterns'].append(show(client,address,0))
        report['final']=status(client,address)
        report['logs']=client.logs().decode(errors='replace')
    if args.output:args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
