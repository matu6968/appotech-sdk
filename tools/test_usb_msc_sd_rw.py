"""Live test of the SD card as a WRITABLE USB disk (examples/usb-modes built with EXAMPLE_DEFS=-DAPX_MODES_SD=2). WRITES TO THE CARD.

    python3 -I tools/test_usb_msc_sd_rw.py REPORT.json --sectors N --allow-write [--mib 16]

--allow-write is required: the owner approved writing to the card in the Silvergear on 2026-11-08. What it writes, and how it puts it back:
  - raw: only the gap between the MBR and the first partition (sectors 1..first_partition-1, at most 62), saved first, restored and compared at the end;
  - files: one scratch file on the card's filesystem, mounted read-write by the desktop (udisks), written with fsync, read back, compared, deleted.
It refuses to run unless the device is the camera's mass-storage disk (USB 1908:3318, "JT5295 SDK Mass Storage"), is not the 1024-sector RAM disk, has the
expected size, is not write-protected for the host, and shows a partition table with such a gap. Ends with fsck.vfat -n on the partition.
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


def pattern(seed, n):
    return bytes((i * 7 + seed * 29 + (i >> 9) * 131 + 1) & 255 for i in range(n))


def main(expect_sectors, mib):
    usb = wait(find_usb, 60)
    if not check("mass-storage device enumerates (1908:3318)", usb):
        return
    product = open(usb + "/product").read().strip()
    name = wait(lambda: find_block(usb), 40)
    if not check("block device appears", name):
        return
    dev = "/dev/" + name
    time.sleep(2)
    probe = os.open(dev, os.O_RDONLY | os.O_DIRECT)
    sectors = os.lseek(probe, 0, os.SEEK_END) // 512
    os.close(probe)
    if not check("is the camera's SD disk (refusing to write otherwise)", product == "JT5295 SDK Mass Storage" and sectors == expect_sectors and sectors != 1024,
                 f"{dev}: '{product}', {sectors} sectors"):
        return
    ro = open(f"/sys/block/{name}/ro").read().strip()
    if not check("host sees the disk as writable (sysfs ro=0)", ro == "0", f"ro={ro}"):
        return
    # unmount anything the desktop mounted: the raw phase must not race a mounted filesystem
    for e in sorted(os.listdir(f"/sys/block/{name}")):
        if e.startswith(name) and e[len(name):].isdigit():
            if mountpoint("/dev/" + e)[0]:
                subprocess.run(["udisksctl", "unmount", "-b", "/dev/" + e], capture_output=True, text=True)
    time.sleep(1)
    buf = mmap.mmap(-1, 1 << 20)
    fd = os.open(dev, os.O_RDWR | os.O_DIRECT | os.O_SYNC)

    def read(lba, count):
        got = os.preadv(fd, [memoryview(buf)[: count * 512]], lba * 512)
        if got != count * 512:
            raise IOError(f"short read {got} at {lba}")
        return bytes(buf[: count * 512])

    def write(lba, data):
        assert len(data) % 512 == 0 and len(data) <= len(buf)
        buf[: len(data)] = data
        if os.pwritev(fd, [memoryview(buf)[: len(data)]], lba * 512) != len(data):
            raise IOError(f"short write at {lba}")

    mbr = read(0, 1)
    first = int.from_bytes(mbr[454:458], "little")
    if not check("partition table: 55AA and a partition that leaves a gap after the MBR", mbr[510:512] == b"\x55\xaa" and 2 <= first <= 63, f"first partition at LBA {first}"):
        return
    gap = first - 1
    saved = read(1, gap)
    mbr_before = mbr
    head_before = read(0, first)  # MBR and gap
    # raw writes inside the gap only
    ok = True
    for seed, count in ((1, 1), (2, 2), (3, 5), (4, 16), (5, gap)):
        count = min(count, gap)
        data = pattern(seed, count * 512)
        write(1, data)
        ok &= read(1, count) == data
    check("raw writes of 1, 2, 5, 16 and the whole gap read back identical", ok)
    ok = True
    for s in range(1, gap + 1):  # single sectors one after the other: the camera runs them as one write stream
        write(s, pattern(9, 512))
    ok = read(1, gap) == pattern(9, 512) * gap
    check("sector-by-sector writes over the whole gap read back identical", ok)
    write(1, saved)
    check("gap restored to the saved bytes", read(1, gap) == saved and read(0, 1) == mbr_before)
    os.close(fd)

    # a scratch file through the desktop's read-write mount
    part = wait(lambda: next((f"/dev/{e}" for e in sorted(os.listdir(f"/sys/block/{name}")) if e.startswith(name) and e[len(name):].isdigit()), None), 20)
    if not check("kernel exposes the partition", part, str(part)):
        return
    mp, opts = mountpoint(part)
    if not mp:
        r = subprocess.run(["udisksctl", "mount", "-b", part, "-o", "rw"], capture_output=True, text=True)
        time.sleep(1)
        mp, opts = mountpoint(part)
    if not check("mounted read-write", mp is not None and "rw" in opts, f"{mp} {opts[:2]}"):
        return
    path = os.path.join(mp, "APXRWTST.BIN")
    if not check("the scratch file does not exist yet", not os.path.exists(path)):
        return
    data = os.urandom(mib << 20)
    sha = hashlib.sha256(data).hexdigest()
    t0 = time.time()
    with open(path, "wb") as f:
        for off in range(0, len(data), 1 << 20):
            f.write(data[off: off + (1 << 20)])
        f.flush()
        os.fsync(f.fileno())
    rate = len(data) / (time.time() - t0) / 1e6
    check(f"{mib} MiB file written with fsync", os.path.getsize(path) == len(data), f"{rate:.2f} MB/s")
    # many small files
    small = {f"APXS{i:03d}.TMP": os.urandom(1000 + 37 * i) for i in range(40)}
    for n, d in small.items():
        with open(os.path.join(mp, n), "wb") as f:
            f.write(d)
    os.sync()
    subprocess.run(["udisksctl", "unmount", "-b", part], capture_output=True, text=True)
    time.sleep(1)
    r = subprocess.run(["udisksctl", "mount", "-b", part, "-o", "ro"], capture_output=True, text=True)
    time.sleep(1)
    mp, opts = mountpoint(part)
    if not check("remounted (read-only) after unmount, i.e. the data went to the card", mp is not None and "ro" in opts, f"{mp}"):
        return
    path = os.path.join(mp, "APXRWTST.BIN")
    t0 = time.time()
    got = open(path, "rb").read()
    rrate = len(got) / (time.time() - t0) / 1e6
    check(f"the {mib} MiB file reads back with the same SHA-256", hashlib.sha256(got).hexdigest() == sha, f"{rrate:.2f} MB/s")
    check("40 small files read back identical", all(open(os.path.join(mp, n), "rb").read() == d for n, d in small.items()))
    # delete them again (rw)
    subprocess.run(["udisksctl", "unmount", "-b", part], capture_output=True, text=True)
    time.sleep(1)
    subprocess.run(["udisksctl", "mount", "-b", part, "-o", "rw"], capture_output=True, text=True)
    time.sleep(1)
    mp, opts = mountpoint(part)
    if mp and "rw" in opts:
        for n in list(small) + ["APXRWTST.BIN"]:
            try:
                os.remove(os.path.join(mp, n))
            except OSError:
                pass
        os.sync()
        gone = not any(os.path.exists(os.path.join(mp, n)) for n in list(small) + ["APXRWTST.BIN"])
        check("the scratch files are deleted again", gone)
    else:
        check("remounted read-write to delete the scratch files", False, str(mp))
    subprocess.run(["udisksctl", "unmount", "-b", part], capture_output=True, text=True)
    time.sleep(1)
    fsck = subprocess.run(["fsck.vfat", "-n", part], capture_output=True, text=True)
    last = fsck.stdout.strip().splitlines()[-1] if fsck.stdout.strip() else fsck.stderr.strip()[:100]
    clean = fsck.returncode == 0 or ("Dirty bit" in fsck.stdout and "differences" not in fsck.stdout.lower())
    check("fsck.vfat -n finds no structural errors", clean, last)
    fd = os.open(dev, os.O_RDONLY | os.O_DIRECT)
    got = os.preadv(fd, [memoryview(buf)[: 512 * first]], 0)
    check("MBR and gap unchanged at the end", got == 512 * first and bytes(buf[: 512 * first]) == head_before)
    os.close(fd)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("report")
    ap.add_argument("--sectors", type=int, required=True)
    ap.add_argument("--mib", type=int, default=16)
    ap.add_argument("--allow-write", action="store_true")
    a = ap.parse_args()
    if not a.allow_write:
        sys.exit("refusing to run: this test writes to the card; pass --allow-write")
    crashed = None
    try:
        main(a.sectors, a.mib)
    except BaseException as exc:
        crashed = exc
        check("the test ran to the end without an exception", False, f"{type(exc).__name__}: {exc}")
    finally:
        ok = all(r["ok"] for r in results) and bool(results) and crashed is None
        print("RESULT:", "ALL PASS" if ok else "FAILURES", f"({sum(r['ok'] for r in results)}/{len(results)})")
        json.dump(results, open(a.report, "w"), indent=1)
    if not ok:
        sys.exit(1)
