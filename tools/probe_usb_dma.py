#!/usr/bin/env python3
"""Host side of examples/usb-dma-probe: read EP1 IN (0x81) of the SDK debug device and check the pattern of every packet.
The pattern for a send of N bytes is byte[i] = (i * 13 + N) & 255, i counted from the start of that send.
Prints one line per packet. Read-only on the host side except the bulk reads."""
import os, sys, time
import usb.core, usb.util

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
quiet = 0
n = 0
start = time.time()
while quiet < 6:
    try:
        data = bytes(dev.read(0x81, 512, timeout=1500))
    except usb.core.USBTimeoutError:
        quiet += 1 if n else 0
        continue
    except usb.core.USBError as e:
        print("USB error", e); break
    quiet = 0
    n += 1
    if os.environ.get('DUMP_DIR'): open(os.path.join(os.environ['DUMP_DIR'], f'pkt-{n}.bin'), 'wb').write(data)
    matches = [f'k{k}L{L}' for k, m in ((1, 13), (2, 7)) for L in range(0, 1100) if len(data) and all(data[i] == (i * m + L) & 255 for i in range(len(data)))]
    print(f"{time.time()-start:6.2f}s packet {n}: {len(data)} bytes, first {data[:6].hex()}, pattern {matches[:3] or 'NONE'}", flush=True)
print("reader done,", n, "packets")
time.sleep(8)
for k, size in enumerate((512, 100, 512, 64, 1, 512), 1):
    pkt = bytes(((k * 37 + i) % 255) + 1 for i in range(size))
    if k in (3, 4):
        pass  # sent back to back with the next one
    dev.write(0x01, pkt, timeout=2000)
    print(f"sent OUT packet {k}: {size} bytes starting {pkt[:4].hex()}", flush=True)
    if k not in (3,):
        time.sleep(1)
