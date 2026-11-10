#!/usr/bin/env python3
"""Host side of examples/usb-ep-probe: read bulk IN endpoints 0x82..0x86 concurrently and check every packet against the pattern
byte[i] = (i * (ep + 3) + ep) & 255. Only reads. Runs for 45 s."""
import sys, threading, time
import usb.core
dev = None
end = time.time() + 60
while dev is None and time.time() < end:
    dev = usb.core.find(idVendor=0x1908, idProduct=0x3319)
    time.sleep(0.2)
if dev is None:
    sys.exit("no SDK debug device")
try:
    dev.set_configuration()
except usb.core.USBError:
    pass
print("config:", [(i.bInterfaceNumber, [hex(e.bEndpointAddress) for e in i]) for i in dev.get_active_configuration()], flush=True)
t0 = time.time()
lock = threading.Lock()
def reader(ep):
    while time.time() - t0 < 45:
        try:
            data = bytes(dev.read(0x80 | ep, 512, timeout=1000))
        except usb.core.USBTimeoutError:
            continue
        except usb.core.USBError as e:
            with lock: print(f"ep{ep}: {e}", flush=True)
            time.sleep(0.5); continue
        good = all(data[i] == (i * (ep + 3) + ep) & 255 for i in range(len(data)))
        with lock: print(f"{time.time()-t0:6.2f}s ep{ep}: {len(data)} bytes, first {data[:6].hex()}, pattern {'OK' if good else 'MISMATCH'}", flush=True)
threads = [threading.Thread(target=reader, args=(ep,)) for ep in (2, 3, 4)]
for t in threads: t.start()
for t in threads: t.join()
