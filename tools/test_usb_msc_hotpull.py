"""Hot-removal of the SD card while the host writes through the camera's writable mass-storage disk (examples/usb-modes built with
EXAMPLE_DEFS=-DAPX_MODES_SD=2 -DAPX_MODES_MSC_SECONDS=...). WRITES TO THE CARD, only to the gap between the MBR and the first partition (saved first,
restored at the end), so no filesystem on the card can be damaged by it.

    python3 -I tools/test_usb_msc_hotpull.py REPORT.json --sectors N --allow-write [--seconds 240]

For --seconds it writes generations of the gap (pattern per generation, read back and compared each time) and logs every I/O error with its errno and how
long the call took. Pull and reinsert the card as often as you like meanwhile. At the end it reports: errors, how long the host needed to see the disk work
again after each reinsertion, whether the device node survived, whether every gap sector holds a complete generation (nothing torn), and restores the gap.
"""
import argparse, errno, json, mmap, os, sys, time

VID, PID = "1908", "3318"


def find_usb():
    base = "/sys/bus/usb/devices"
    for d in os.listdir(base):
        p = os.path.join(base, d)
        try:
            if open(p + "/idVendor").read().strip() == VID and open(p + "/idProduct").read().strip() == PID:
                return os.path.realpath(p)
        except OSError:
            pass


def find_block(usb):
    for b in sorted(os.listdir("/sys/block")):
        if b.startswith("sd") and os.path.realpath(f"/sys/block/{b}/device").startswith(usb + "/"):
            return b


def pattern(gen, lba):
    return bytes(((i * 7) + gen * 29 + lba * 131 + 1) & 255 for i in range(512))


def main(expect, seconds, report):
    usb = find_usb()
    if not usb:
        sys.exit("no 1908:3318 device")
    name = find_block(usb)
    if not name:
        sys.exit("no block device")
    dev = "/dev/" + name
    product = open(usb + "/product").read().strip()
    fd = os.open(dev, os.O_RDWR | os.O_DIRECT | os.O_SYNC)
    sectors = os.lseek(fd, 0, os.SEEK_END) // 512
    if not (product == "JT5295 SDK Mass Storage" and sectors == expect and sectors != 1024 and open(f"/sys/block/{name}/ro").read().strip() == "0"):
        sys.exit(f"refusing: {dev} '{product}' {sectors} sectors")
    buf = mmap.mmap(-1, 1 << 16)

    def rd(lba, n):
        got = os.preadv(fd, [memoryview(buf)[: n * 512]], lba * 512)
        if got != n * 512:
            raise OSError(errno.EIO, f"short read {got}")
        return bytes(buf[: n * 512])

    def wr(lba, data):
        buf[: len(data)] = data
        if os.pwritev(fd, [memoryview(buf)[: len(data)]], lba * 512) != len(data):
            raise OSError(errno.EIO, "short write")

    mbr = rd(0, 1)
    first = int.from_bytes(mbr[454:458], "little")
    if not (mbr[510:512] == b"\x55\xaa" and first >= 2 and mbr[450] != 0xee):
        sys.exit("no MBR gap")
    gap = min(first - 1, 62)
    saved = rd(1, gap)
    if any(saved):
        sys.exit("refusing: the gap sectors are not all zero")
    print(f"{dev}: gap sectors 1..{gap}; running {seconds} s; pull and reinsert the card as often as you like", flush=True)
    events, gen, ok_after_error = [], 0, None
    last_error_t, recoveries = None, []
    t_end = time.time() + seconds
    while time.time() < t_end:
        t0 = time.time()
        try:
            wr(1, b"".join(pattern(gen, 1 + s) for s in range(gap)))
            back = rd(1, gap)
            if back != b"".join(pattern(gen, 1 + s) for s in range(gap)):
                events.append((t0, "MISMATCH after a successful write", time.time() - t0))
            else:
                gen += 1
                if last_error_t is not None:
                    recoveries.append(time.time() - last_error_t)
                    last_error_t = None
        except OSError as e:
            dt = time.time() - t0
            events.append((t0, f"errno {e.errno} {errno.errorcode.get(e.errno, '?')}", dt))
            if last_error_t is None:
                last_error_t = t0
            print(f"{time.time() - (t_end - seconds):7.1f}s I/O error {e.errno} after {dt * 1000:.0f} ms", flush=True)
            time.sleep(0.5)
    node_ok = os.path.exists(dev) and find_usb() is not None
    # final state: wait for the disk to work, classify the gap, restore it
    final = None
    deadline = time.time() + 60
    while time.time() < deadline:
        try:
            final = rd(1, gap)
            break
        except OSError:
            time.sleep(1)
    cls = {"latest": 0, "previous": 0, "older": 0, "TORN/other": 0}
    if final is not None:
        for s in range(gap):
            sec = final[s * 512:(s + 1) * 512]
            for g, key in ((gen, "latest"), (gen - 1, "previous")):
                if g >= 0 and sec == pattern(g, 1 + s):
                    cls[key] += 1
                    break
            else:
                if any(sec == pattern(g, 1 + s) for g in range(max(0, gen - 50), gen - 1)):
                    cls["older"] += 1
                else:
                    cls["TORN/other"] += 1
    restored = False
    if final is not None:
        try:
            wr(1, saved)
            restored = rd(1, gap) == saved
        except OSError:
            pass
    res = {"generations_completed": gen, "io_errors": len(events), "error_kinds": sorted({e[1] for e in events}), "longest_failing_call_ms": round(max([e[2] for e in events] + [0]) * 1000),
           "recoveries_s": [round(r, 1) for r in recoveries], "device_node_still_there": node_ok, "gap_classification": cls, "gap_restored": restored}
    print(json.dumps(res, indent=1))
    json.dump(res, open(report, "w"), indent=1)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("report")
    ap.add_argument("--sectors", type=int, required=True)
    ap.add_argument("--seconds", type=int, default=240)
    ap.add_argument("--allow-write", action="store_true")
    a = ap.parse_args()
    if not a.allow_write:
        sys.exit("refusing to run: this test writes to the card (the gap after the MBR); pass --allow-write")
    main(a.sectors, a.seconds, a.report)
