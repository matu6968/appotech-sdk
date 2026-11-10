"""Live test of the SDK's read-only SD card mass-storage mode (examples/usb-modes built with EXAMPLE_DEFS=-DAPX_MODES_SD=1).

STRICTLY READ-ONLY: the block device is only ever opened O_RDONLY, files are compared through a read-only mount, and write
protection is checked from sysfs, never by attempting a write. It refuses to run unless the device is the camera's mass-storage
disk (USB 1908:3318, product "JT5295 SDK Mass Storage") and is not the 1024-sector RAM disk.

Reference data comes from the debug link while the camera is still in debug mode (apxtool debug sd get / read-sector), in REFS:
    SDKTEST.BIN, photo.jpg + photo.name (the file name under PHOTO/), sector-<lba>.bin (512 bytes each)
    python3 -I tools/test_usb_msc_sd.py REFS REPORT.json --sectors 3862528

Needs: membership of the `disk` group, udisksctl, fsck.vfat.
"""
import argparse, hashlib, json, mmap, os, subprocess, sys, time

VID, PID = "1908", "3318"
results = []


def check(name, ok, detail=""):
    results.append({"check": name, "ok": bool(ok), "detail": detail})
    print(("PASS " if ok else "FAIL ") + name + (f" - {detail}" if detail else ""), flush=True)
    return ok


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


def wait(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        r = fn()
        if r:
            return r
        time.sleep(0.25)


def mountpoint(dev):
    for line in open("/proc/mounts"):
        f = line.split()
        if f[0] == dev:
            return f[1].replace("\\040", " "), f[3].split(",")
    return None, []


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main(refs, expect_sectors):
    usb = wait(find_usb, 60)
    if not check("mass-storage device enumerates (1908:3318)", usb):
        return
    product = open(usb + "/product").read().strip()
    name = wait(lambda: find_block(usb), 40)
    if not check("block device appears", name):
        return
    dev = "/dev/" + name
    time.sleep(2)
    fd = os.open(dev, os.O_RDONLY | os.O_DIRECT)  # the ONLY way this script ever opens the device
    sectors = os.lseek(fd, 0, os.SEEK_END) // 512
    if not check("is the camera's disk and not the RAM disk (refusing to continue otherwise)",
                 product == "JT5295 SDK Mass Storage" and sectors != 1024,
                 f"{dev}: '{product}', {sectors} sectors"):
        return
    check("capacity equals the card's size", sectors == expect_sectors, f"{sectors} sectors = {sectors * 512 / 2**30:.2f} GiB")
    ro = open(f"/sys/block/{name}/ro").read().strip()
    check("host sees the disk as write-protected (sysfs ro=1)", ro == "1", f"ro={ro}")
    buf = mmap.mmap(-1, 1 << 20)

    def read(lba, count):
        out = b""
        while count:
            n = min(count, 256)
            got = os.preadv(fd, [memoryview(buf)[: n * 512]], lba * 512)
            if got != n * 512:
                raise IOError(f"short read {got} at lba {lba}")
            out += buf[: n * 512]
            lba += n
            count -= n
        return out

    # The card is partitioned: sector 0 is an MBR and the FAT16 volume starts at the first partition's LBA.
    mbr = read(0, 1)
    ptype, start = mbr[450], int.from_bytes(mbr[454:458], "little")
    check("MBR: 55AA and a FAT16 first partition", mbr[510:512] == b"\x55\xaa" and ptype in (0x04, 0x06, 0x0E),
          f"type {ptype:#04x}, starts at LBA {start}")
    boot = read(start, 1)
    # The FAT type is defined by the cluster count, not by the informational text at offset 54 (often just "FAT").
    bps = int.from_bytes(boot[11:13], "little")
    spc, reserved, fats = boot[13], int.from_bytes(boot[14:16], "little"), boot[16]
    root_entries = int.from_bytes(boot[17:19], "little")
    total = int.from_bytes(boot[19:21], "little") or int.from_bytes(boot[32:36], "little")
    fat_size = int.from_bytes(boot[22:24], "little")
    root_sectors = (root_entries * 32 + bps - 1) // bps if bps else 0
    clusters = (total - reserved - fats * fat_size - root_sectors) // spc if spc and bps == 512 else 0
    check("partition boot sector: 55AA, 512-byte sectors, FAT16 by cluster count (4085..65524)",
          boot[510:512] == b"\x55\xaa" and bps == 512 and 4085 <= clusters < 65525,
          f"{clusters} clusters of {spc} sectors, {total} sectors, fs-type text {boot[54:62]!r}")
    part = wait(lambda: next((f"/dev/{e}" for e in sorted(os.listdir(f"/sys/block/{name}")) if e.startswith(name) and e[len(name):].isdigit()), None), 20)
    if not check("kernel exposes the partition", part, str(part)):
        return

    # raw sectors against the references taken over the debug link
    bad, n = [], 0
    for f in sorted(os.listdir(refs)):
        if f.startswith("sector-") and f.endswith(".bin"):
            lba = int(f[7:-4])
            n += 1
            if lba < sectors and read(lba, 1) != open(os.path.join(refs, f), "rb").read():
                bad.append(lba)
    check(f"{n} raw sectors (first, middle, last) equal the debug-link reads", n > 0 and not bad, f"mismatches: {bad}")

    # stability and chunking: the same range twice, bulk and sector by sector
    a = read(100000, 2048)
    b = read(100000, 2048)
    c = b"".join(read(100000 + i, 1) for i in range(0, 256))
    check("1 MiB read twice is identical; 256 single-sector reads equal the bulk read", a == b and c == a[: 256 * 512])

    # throughput
    t = time.time()
    total = 0
    for i in range(32):
        total += len(read(200000 + 256 * i, 256))
    rate = total / (time.time() - t) / 1e6
    check("sequential read throughput measured", True, f"{rate:.2f} MB/s over {total >> 20} MiB (sequential reads; the SD driver streams them with one CMD18)")

    # filesystem view through a read-only mount (never writes)
    mp, opts = mountpoint(part)
    if not mp:
        r = subprocess.run(["udisksctl", "mount", "-b", part, "-o", "ro"], capture_output=True, text=True)
        time.sleep(1)
        mp, opts = mountpoint(part)
    if check("mounted, read-only, volume label APX SDK", mp is not None and "ro" in opts and mp.endswith("APX SDK"), f"{mp} opts={','.join(opts[:3])}"):
        for rel, ref in (("SDKTEST.BIN", "SDKTEST.BIN"), ("PHOTO/" + open(os.path.join(refs, "photo.name")).read().strip(), "photo.jpg")):
            got = open(os.path.join(mp, rel), "rb").read()
            want = open(os.path.join(refs, ref), "rb").read()
            check(f"file {rel} ({len(want)} bytes) equals the debug-link copy", sha(got) == sha(want))
        names = sorted(os.listdir(mp))
        check("directory listing has the card's folders", {"PHOTO", "VIDEO"} <= set(names), str(names)[:100])
    fsck = subprocess.run(["fsck.vfat", "-n", part], capture_output=True, text=True)
    last = fsck.stdout.strip().splitlines()[-1] if fsck.stdout.strip() else fsck.stderr.strip()[:100]
    clean_enough = fsck.returncode == 0 or ("Dirty bit" in fsck.stdout and "differences" not in fsck.stdout.lower())
    check("fsck.vfat -n finds no structural errors", clean_enough, last)
    os.close(fd)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("refs")
    ap.add_argument("report")
    ap.add_argument("--sectors", type=int, required=True)
    a = ap.parse_args()
    crashed = None
    try:
        main(a.refs, a.sectors)
    except BaseException as exc:  # a crash is a failure, not a pass of whatever ran before it
        crashed = exc
        check("the test ran to the end without an exception", False, f"{type(exc).__name__}: {exc}")
    finally:
        ok = all(r["ok"] for r in results) and bool(results) and crashed is None
        print("RESULT:", "ALL PASS" if ok else "FAILURES", f"({sum(r['ok'] for r in results)}/{len(results)})")
        json.dump(results, open(a.report, "w"), indent=1)
    if not ok:
        sys.exit(1)
