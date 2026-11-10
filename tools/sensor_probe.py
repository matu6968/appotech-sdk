#!/usr/bin/env python3
"""Host side of examples/sensor-probe: read, write and dump the ID 0x39 sensor's registers and measure the frame interval, live over the APXD debug link.

    PYTHONPATH=../apxtool/src python3 tools/sensor_probe.py dump 0 [--ref FILE]
    python3 tools/sensor_probe.py read 1 0x03 | write 0 0x24 0x10 | stats [seconds] | select 0|1 | save OUT.json
Only talks to the camera whose USB identity is 1908:3319 (the SDK debug device); never touches storage."""
import json, os, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "apxtool", "src"))
from apxtool.debug import DebugClient, USBTransport, Operation

TICKS = 32768.0


def port():
    base = "/sys/bus/usb/devices"
    want = os.environ.get("APX_USB_PORT")  # with more than one camera attached, name the port (e.g. 5-1)
    for d in sorted(os.listdir(base)):
        if want and d != want:
            continue
        try:
            if open(f"{base}/{d}/idVendor").read().strip() == "1908" and open(f"{base}/{d}/idProduct").read().strip() == "3319":
                return d
        except OSError:
            pass
    sys.exit("no SDK debug device (1908:3319)")


class Probe:
    def __init__(self):
        self.t = USBTransport(port=port())
        self.c = DebugClient(self.t)

    def cmd(self, command, *words):
        payload = b"".join(struct.pack("<I", w) for w in words)
        return self.c.request(Operation.CAMERA, address=command, length=len(payload), payload=payload)

    def read(self, page, reg):
        return struct.unpack("<I", self.cmd(0x20, page, reg)[:4])[0]

    def write(self, page, reg, value):
        self.cmd(0x21, page, reg, value)

    def dump(self, page):
        raw = self.cmd(0x22, page)
        return [struct.unpack_from("<H", raw, 2 * i)[0] for i in range(256)]

    def stats(self):
        f, last, avg, mn, mx, err = struct.unpack("<6I", self.cmd(0x23)[:24])
        return dict(frames=f, last_ms=last * 1000 / TICKS, avg_ms=avg * 1000 / TICKS, min_ms=mn * 1000 / TICKS, max_ms=mx * 1000 / TICKS, fps=(TICKS / avg if avg else 0), errors=err)

    def luma(self):
        m, mn, mx, sd, chroma, u, v = struct.unpack("<7I", self.cmd(0x26)[:28])
        return dict(mean=m, min=mn, max=mx, sd=sd, chroma=chroma, u=u, v=v)

    def encode(self, selector, quality=None):
        """Encode one clean frame with the hardware JPEG engine at the given quantizer selector: (bytes, encode ms, jpeg data)."""
        n, ticks, addr = struct.unpack("<3I", self.cmd(0x27, selector, *( [quality] if quality is not None else [] ))[:12])
        return n, ticks * 1000 / TICKS, self.c.read(addr, n)

    def reset(self):
        self.cmd(0x24)

    def select(self, cam):
        self.cmd(0x25, cam)

    def measure(self, seconds=3.0):
        """Frame interval over `seconds` after a statistics reset (the first frames after a register change are discarded)."""
        self.reset(); time.sleep(0.5); self.reset(); time.sleep(seconds)
        return self.stats()


def show_dump(page, vals):
    print(f"page {page}:")
    for r in range(0, 256, 16):
        print(f"  {r:02x}: " + " ".join("--" if v == 0xFFFF else f"{v:02x}" for v in vals[r:r + 16]))


if __name__ == "__main__":
    p = Probe()
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    n = lambda s: int(s, 0)
    if a[0] == "dump":
        show_dump(n(a[1]), p.dump(n(a[1])))
    elif a[0] == "read":
        print(hex(p.read(n(a[1]), n(a[2]))))
    elif a[0] == "write":
        p.write(n(a[1]), n(a[2]), n(a[3])); print("ok")
    elif a[0] == "stats":
        print(p.measure(float(a[1]) if len(a) > 1 else 3.0))
    elif a[0] == "encode":
        for sel in ([n(x) for x in a[1:]] or range(16)):
            size, ms, data = p.encode(sel)
            print(f"selector {sel:2d}: {size:6d} bytes, {ms:.1f} ms")
    elif a[0] == "luma":
        print(p.luma())
    elif a[0] == "select":
        p.select(n(a[1])); print("ok")
    elif a[0] == "save":
        out = {str(pg): p.dump(pg) for pg in range(3)}
        json.dump(out, open(a[1], "w")); print("saved", a[1])
    else:
        sys.exit(__doc__)
