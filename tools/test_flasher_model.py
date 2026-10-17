#!/usr/bin/env python3
"""Runs boot/flasher.c on the host against a modelled SPI NOR, PM chip, watchdog and data cache.

Checks the command set from both memory pools: the SRAM window (512-byte transfers, uncached) and the DDR window
(32 KiB transfers, cache maintenance around every request). apxtool/tests/test_flasher.py runs the real host client
against the same model."""
import ctypes,struct,subprocess,tempfile
from pathlib import Path
sdk=Path(__file__).resolve().parents[1]
HEADER=struct.Struct('<7I');MAGIC=0x52585041;PENDING=0xffffffff
NOT_DONE=1 # a command refused inside the payload; PENDING means the request never ran (bad magic, window exceeded)
JEDEC,READ,PROGRAM,ERASE,SPR,RAM,SNAPSHOT,PM_ENABLE,PM_READ=range(9)

def build(directory):
    library=Path(directory)/'flasher_model.so'
    subprocess.run(['cc','-std=gnu11','-O1','-fPIC','-shared','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=undefined',
                    '-DFLASHER_HOST',str(sdk/'boot/flasher.c'),str(sdk/'tests/flasher_model.c'),'-o',str(library)],check=True)
    return library

class Model:
    def __init__(self,path):
        self.lib=ctypes.CDLL(str(path))
        self.lib.model_nor.restype=ctypes.POINTER(ctypes.c_uint8*0x200000)
        self.lib.model_pm.restype=ctypes.POINTER(ctypes.c_uint8*0x60)
        self.lib.model_wdt.restype=ctypes.c_uint32;self.lib.model_wdt_reloads.restype=ctypes.c_uint32
        self.lib.model_sync_count.restype=ctypes.c_uint
        self.lib.model_set_sram_window.argtypes=[ctypes.c_void_p,ctypes.c_uint64]
        self.lib.model_call.argtypes=[ctypes.c_void_p]
        self.lib.model_get_spr.restype=ctypes.c_uint32
        self.lib.model_get_spr.argtypes=[ctypes.c_uint]
        self.lib.model_set_spr.argtypes=[ctypes.c_uint,ctypes.c_uint32]
        self.lib.model_sync_get.argtypes=[ctypes.c_uint,ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(ctypes.c_uint32)]
        self.sram=ctypes.create_string_buffer(0x400);self.ddr=ctypes.create_string_buffer(0x9000)
        self.reset()
    def reset(self):
        self.lib.model_reset()
        self.lib.model_set_sram_window(ctypes.addressof(self.sram),len(self.sram))
    @property
    def nor(self):return self.lib.model_nor().contents
    @property
    def pm(self):return self.lib.model_pm().contents
    def syncs(self):
        start,count=ctypes.c_uint64(),ctypes.c_uint32()
        result=[]
        for index in range(self.lib.model_sync_count()):
            self.lib.model_sync_get(index,ctypes.byref(start),ctypes.byref(count));result.append((start.value,count.value))
        return result
    def call(self,buffer,command,address=0,length=0,data=b'',flags=0,magic=MAGIC,sequence=1):
        request=HEADER.pack(magic,sequence,command,address,length,PENDING,flags)+data
        ctypes.memset(buffer,0,len(buffer));ctypes.memmove(buffer,request,len(request))
        self.lib.model_call(ctypes.addressof(buffer))
        raw=bytes(buffer.raw[:HEADER.size+max(length,3)])
        return HEADER.unpack_from(raw)[5],raw[HEADER.size:]

with tempfile.TemporaryDirectory(prefix='jt5295-flasher-') as directory:
    m=Model(build(directory));sram,ddr=m.sram,m.ddr
    # Both pools: JEDEC through the BootROM's SPI state, with the watchdog disabled by default (flags = 0).
    for buffer in (sram,ddr):
        m.reset()
        status,data=m.call(buffer,JEDEC,1,0)
        assert status==0 and data[:3]==bytes.fromhex('5e4015'),(status,data)
        assert m.lib.model_wdt()==0,'default request disables the watchdog'
    m.reset();status,data=m.call(sram,JEDEC,0,0)
    assert status==0 and m.lib.model_get_spr(0x9000)&0x4000,'JEDEC with full setup programs the controller and releases chip select'
    assert m.lib.model_get_spr(0x926c)==0x0fffffff
    # FLAG_KEEP_WDT: a stock camera in its loader keeps its watchdog armed.
    m.reset();status,_=m.call(ddr,JEDEC,1,0,flags=1)
    assert status==0 and m.lib.model_wdt()==0x1234,'an armed watchdog is left alone'
    status,_=m.call(ddr,ERASE,0x1000,0,flags=1)
    assert status==0 and m.lib.model_wdt()==0x1234 and m.lib.model_wdt_reloads()>=1,'busy-waits reload it while armed'
    m.reset();m.call(sram,ERASE,0x1000,0);assert m.lib.model_wdt_reloads()==0,'a disabled watchdog is never re-armed'
    # Window limits: SRAM takes 512 bytes, DDR 32 KiB; anything larger is refused before any flash access.
    m.reset()
    for index in range(0x9000):m.nor[index]=(index*13+5)&0xff
    expected=bytes((index*13+5)&0xff for index in range(0x9000))
    status,data=m.call(sram,READ,0x100,512);assert status==0 and data[:512]==expected[0x100:0x300]
    status,_=m.call(sram,READ,0x100,513);assert status==PENDING,'SRAM window is 512 bytes'
    status,data=m.call(ddr,READ,0,0x8000);assert status==0 and data[:0x8000]==expected[:0x8000]
    status,_=m.call(ddr,READ,0,0x8001);assert status==PENDING,'DDR window is 32 KiB'
    status,_=m.call(ddr,READ,0x1fffff,2);assert status==NOT_DONE,'reads stay inside the 2 MiB chip'
    status,data=m.call(ddr,READ,0x1fffff,1);assert status==0
    # Erase / program / verify, including the busy-wait path and the program-bit semantics of NOR.
    for buffer in (sram,ddr):
        m.reset()
        status,_=m.call(buffer,ERASE,0x5000,0);assert status==0
        status,data=m.call(buffer,READ,0x5000,256);assert data[:256]==b'\xff'*256
        page=bytes((index*7)&0xff for index in range(256))
        status,_=m.call(buffer,PROGRAM,0x5100,256,page);assert status==0
        status,data=m.call(buffer,READ,0x5100,256);assert data[:256]==page
        status,_=m.call(buffer,PROGRAM,0x5100,256,b'\x0f'*256);assert status==0
        status,data=m.call(buffer,READ,0x5100,256);assert data[:256]==bytes(a&0x0f for a in page),'programming only clears bits'
        status,_=m.call(buffer,ERASE,0x5001,0);assert status==NOT_DONE,'erase must be sector aligned'
        status,_=m.call(buffer,ERASE,0x5000,1);assert status==NOT_DONE,'erase carries no data'
        status,_=m.call(buffer,PROGRAM,0x51f0,32,b'\0'*32);assert status==NOT_DONE,'a program may not cross a page'
        status,_=m.call(buffer,PROGRAM,0x5100,0,b'');assert status==NOT_DONE,'an empty program is refused'
        status,_=m.call(buffer,ERASE,0x200000,0);assert status==NOT_DONE,'erase beyond the chip'
        status,_=m.call(buffer,PROGRAM,0x200000-0x100,256,page);assert status==0,'the last page is programmable'
    # PM chip: read, then the stock start-up sequence restores the keep-alive register.
    m.reset();status,data=m.call(sram,PM_READ,0,14);assert status==0 and data[0x5a//2-0x20]==0x00
    status,data=m.call(sram,PM_ENABLE,0,14);assert status==0
    pm=bytes(m.pm);assert pm[0x5a]==0x08 and pm[0x40]==0x16 and pm[0x42]==0xe0 and pm[0x58]==0x0c|0x08,pm.hex()
    status,_=m.call(sram,PM_READ,0,13);assert status==NOT_DONE,'PM reads return exactly 14 registers'
    # Diagnostic SPRs.
    m.reset();m.lib.model_set_spr(0x8858,0x11223344)
    status,data=m.call(sram,SNAPSHOT,0,32);assert status==0 and struct.unpack_from('<I',data,4)[0]==0x11223344
    status,data=m.call(sram,SPR,0x8858,4);assert status==0 and struct.unpack('<I',data[:4])[0]==0x11223344
    status,_=m.call(sram,SPR,0x10000,4);assert status==NOT_DONE
    status,_=m.call(sram,RAM,0x400001,4);assert status==NOT_DONE,'RAM reads stay under 4 MiB'
    # Malformed requests do nothing: a wrong magic leaves the status pending, an unknown command is not "done".
    m.reset();status,_=m.call(sram,READ,0,16,magic=0);assert status==PENDING
    status,_=m.call(sram,9,0,0);assert status==NOT_DONE,'unknown command reports not-done'
    # Cache discipline. Only a DDR block is ever maintained, and only while the loader's data cache is on.
    base=ctypes.addressof(ddr)
    m.reset();m.call(sram,READ,0,512);assert m.syncs()==[],'SRAM requests never touch the cache'
    m.reset();m.lib.model_set_spr(0x1800,0);m.call(ddr,READ,0,64);assert m.syncs()==[],'cache off: no maintenance'
    m.reset();m.call(ddr,READ,0x100,300)
    calls=m.syncs();assert calls[0]==(base,28),calls
    assert calls[-1][0]==base and calls[-1][1]>=28+300,calls
    # Even a request rejected after the status word was written ends with a sync that covers it.
    m.reset();m.call(ddr,PM_READ,0,13);assert m.syncs()[-1][0]==base and m.syncs()[-1][1]>=28+13
    # Layout contract shared with apxtool: 28-byte header, status at offset 20.
    assert HEADER.size==28
print('flasher model: SRAM and DDR windows, NOR semantics, PM sequence, watchdog flag, cache coverage, malformed requests')
