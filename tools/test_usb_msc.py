"""Live test of the SDK's USB mass-storage mode (examples/usb-modes: a 512 KiB FAT12 RAM disk, VID 1908 / PID 3318).

Run it while the camera is in mass-storage mode (the example enters it 8 s after launch and stays 120 s):
    tools/launch.sh ... usb-modes.manifest.json;  python3 -I tools/test_usb_msc.py REPORT.json
The desktop automounts the disk within a second or two. Raw writes under a mounted filesystem corrupt it, so the test
unmounts it first (udisksctl) and checks again after the raw phase that nothing remounted it. Needs: membership of the
`disk` group, udisksctl, mtools, fsck.vfat. Only ever opens the block device whose USB ancestor has that VID:PID. All offsets are checked against the device size
first. I/O is O_DIRECT through page-aligned mmap buffers, so reads and writes really go to the device.
"""
import json, mmap, os, subprocess, sys, time

PAGE = mmap.PAGESIZE
VID, PID = "1908", "3318"
results = []


def check(name, ok, detail="", skipped=False):
    results.append({"check": name, "ok": bool(ok), "detail": detail, "skipped": skipped})
    print(("SKIP " if skipped else "PASS " if ok else "FAIL ") + name + (f" - {detail}" if detail else ""), flush=True)
    return ok


def find_usb(pid):
    base = "/sys/bus/usb/devices"
    for d in os.listdir(base):
        p = os.path.join(base, d)
        try:
            if open(p + "/idVendor").read().strip() == VID and open(p + "/idProduct").read().strip() == pid:
                return os.path.realpath(p)
        except OSError:
            pass
    return None


def find_block(usb):
    for b in sorted(os.listdir("/sys/block")):
        if not b.startswith("sd"):
            continue
        if os.path.realpath(f"/sys/block/{b}/device").startswith(usb + "/"):
            return "/dev/" + b
    return None


def wait(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        r = fn()
        if r:
            return r
        time.sleep(0.25)
    return None


class Disk:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_DIRECT | os.O_SYNC)
        self.size = os.lseek(self.fd, 0, os.SEEK_END)
        self.sectors = self.size // 512
        self.buf = mmap.mmap(-1, 1 << 20)  # 1 MiB, page aligned

    def read(self, lba, count):
        assert 0 <= lba and lba + count <= self.sectors, "read out of range"
        out = b""
        while count:
            n = min(count, 128)
            got = os.preadv(self.fd, [memoryview(self.buf)[: n * 512]], lba * 512)
            if got != n * 512:
                raise IOError(f"short read {got} at lba {lba}")
            out += self.buf[: n * 512]
            lba += n
            count -= n
        return out

    def write(self, lba, data):
        assert len(data) % 512 == 0 and 0 <= lba and lba + len(data) // 512 <= self.sectors, "write out of range"
        done = 0
        while done < len(data):
            n = min(len(data) - done, 64 * 512)
            self.buf[:n] = data[done : done + n]
            put = os.pwritev(self.fd, [memoryview(self.buf)[:n]], lba * 512 + done)
            if put != n:
                raise IOError(f"short write {put} at lba {lba + done // 512}")
            done += n


def main():
    usb = wait(lambda: find_usb(PID), 40)
    if not check("MSC device enumerates (1908:3318)", usb):
        return
    speed = open(usb + "/speed").read().strip()
    check("high speed", speed == "480", f"{speed} Mbps; product '{open(usb + '/product').read().strip()}'")
    dev = wait(lambda: find_block(usb), 30)
    if not check("block device appears", dev):
        return
    time.sleep(2)  # the desktop automounts within a second or two
    for _ in range(10):
        mounted = [l.split()[1] for l in open("/proc/mounts") if l.split()[0] == dev]
        if not mounted:
            break
        r = subprocess.run(["udisksctl", "unmount", "-b", dev], capture_output=True, text=True)
        time.sleep(0.5)
    check("not mounted during raw I/O (a mounted FS would be corrupted by raw writes)",
          not [l for l in open("/proc/mounts") if l.split()[0] == dev], f"unmounted {mounted}" if mounted else "was not mounted")
    d = Disk(dev)
    # SAFETY: every later step writes random data. Only the built-in RAM disk may be written; anything else (an SD card
    # presented by APX_MODES_SD=1, say) must never be.
    head = d.read(0, 1)  # through the aligned O_DIRECT buffer: an unaligned buffer fails with EINVAL
    is_ramdisk = d.sectors == 1024 and head[43:54] == b"APXSDK RAM " and open(usb + "/product").read().strip() == "JT5295 SDK Mass Storage"
    if not check("this is the built-in 1024-sector RAM disk (the test writes to the device)", is_ramdisk,
                 f"{dev}: {d.sectors} sectors, label {head[43:54]!r}" + ("" if is_ramdisk else "; refusing to write")):
        os.close(d.fd)
        return

    def mount_note(tag):
        m = [l.split()[1] for l in open("/proc/mounts") if l.split()[0] == dev]
        if m and tag not in seen:
            seen.append(tag)
        return m
    seen = []
    mount_note("after open")
    # 1. reads: stable, and identical whatever the chunking
    whole1 = d.read(0, 1024)
    whole2 = d.read(0, 1024)
    check("two full reads identical", whole1 == whole2)
    chunked = b"".join(d.read(i, 1) for i in range(0, 1024, 1))
    check("sector-at-a-time read equals bulk read", chunked == whole1)
    check("boot sector signature 0x55AA and FAT12 label", whole1[510:512] == b"\x55\xaa" and whole1[54:61] == b"FAT12  ")

    # 2. write/readback with random data at assorted sizes and alignments, strictly inside the free area (sectors 100..999)
    rnd = os.urandom
    cases = [(100, 1), (101, 2), (103, 3), (110, 7), (120, 8), (130, 63), (200, 64), (300, 65), (400, 127), (550, 128), (700, 129), (100, 800)]
    bad = []
    for lba, n in cases:
        data = rnd(n * 512)
        d.write(lba, data)
        if d.read(lba, n) != data:
            bad.append((lba, n))
    check("write/readback, 12 size/alignment cases (1..800 sectors)", not bad, f"mismatches: {bad}")

    mount_note("after step 2")
    # 3. neighbours untouched by a one-sector write
    d.write(500, b"\0" * 512 * 3)
    before = d.read(499, 5)
    marker = rnd(512)
    d.write(501, marker)
    after = d.read(499, 5)
    check("one-sector write leaves neighbours intact", after[:1024] == before[:1024] and after[1024:1536] == marker and after[1536:] == before[1536:])

    mount_note("after step 3")
    # 4. throughput
    t = time.time()
    for _ in range(8):
        d.read(0, 1024)
    rd = 8 * 512 * 1024 / (time.time() - t) / 1e6
    blk = rnd(256 * 512)
    t = time.time()
    for _ in range(8):
        d.write(200, blk)
    wr = 8 * len(blk) / (time.time() - t) / 1e6
    check("throughput measured", True, f"read {rd:.2f} MB/s, write {wr:.2f} MB/s (RAM disk, 512 byte reads/writes, polled PIO)")

    mount_note("after step 4")
    # 5. a filesystem on top: copy a file in and out through mtools, fsck
    d.write(0, whole1[:512])  # no-op write of sector 0 (also proves boot-sector writes work)
    subprocess.run(["sync"])
    # restore the free area to zeros so the FS view is clean
    d.write(100, b"\0" * 512 * 900)
    f = rnd(60000)
    open("/tmp/_msc_file", "wb").write(f)
    r1 = subprocess.run(["mcopy", "-D", "o", "-i", dev, "/tmp/_msc_file", "::DATA.BIN"], capture_output=True, text=True)
    r2 = subprocess.run(["mcopy", "-i", dev, "::DATA.BIN", "/tmp/_msc_back"], capture_output=True, text=True)
    same = os.path.exists("/tmp/_msc_back") and open("/tmp/_msc_back", "rb").read() == f
    check("file copy in and out through the FAT (60000 bytes)", r1.returncode == 0 and r2.returncode == 0 and same, (r1.stderr + r2.stderr).strip()[:160])
    fsck = subprocess.run(["fsck.vfat", "-n", dev], capture_output=True, text=True)
    check("fsck.vfat -n clean (unmounted)", fsck.returncode == 0, fsck.stdout.strip().splitlines()[-1] if fsck.stdout.strip() else fsck.stderr.strip()[:120])
    for p in ("/tmp/_msc_file", "/tmp/_msc_back"):
        try:
            os.unlink(p)
        except OSError:
            pass
    check("README.TXT still readable", b"RAM disk" in subprocess.run(["mtype", "-i", dev, "::README.TXT"], capture_output=True).stdout)
    mount_note("after step 5")
    check("first seen mounted", not seen, f"first at: {seen[:1]}" if seen else "never during raw phase")
    still = [l.split()[1] for l in open("/proc/mounts") if l.split()[0] == dev]
    check("still not mounted when the raw phase ended (no raw write raced a mount)", not still, f"mounted at {still}" if still else "")
    os.close(d.fd)

    # 6. realistic use: mount it like the desktop does and let the kernel's FAT driver write a file
    r = subprocess.run(["udisksctl", "mount", "-b", dev], capture_output=True, text=True)
    mp = next((l.split()[1].replace("\\040", " ") for l in open("/proc/mounts") if l.split()[0] == dev), None)
    if check("mounts through udisks", mp is not None, (r.stdout + r.stderr).strip()[:140]):
        big = rnd(200_000)
        with open(os.path.join(mp, "KERNEL.BIN"), "wb") as fh:
            fh.write(big)
            fh.flush()
            os.fsync(fh.fileno())
        subprocess.run(["sync"])
        check("file written by the kernel FAT driver reads back (200000 bytes)", open(os.path.join(mp, "KERNEL.BIN"), "rb").read() == big)
        os.unlink(os.path.join(mp, "KERNEL.BIN"))
        names = sorted(os.listdir(mp))
        check("directory listing after the write has no stray entries", all(n in ("README.TXT", "DATA.BIN") or n.startswith(".") for n in names), str(names))
        for _ in range(20):  # the unmount can take a moment, or fail while a file manager holds the volume open
            subprocess.run(["udisksctl", "unmount", "-b", dev], capture_output=True)
            if not [l for l in open("/proc/mounts") if l.split()[0] == dev]:
                break
            time.sleep(0.5)
        if [l for l in open("/proc/mounts") if l.split()[0] == dev]:
            check("fsck after the kernel's own writes", True, "volume could not be unmounted (held open by another program); a mounted FS always shows the dirty bit", skipped=True)
        else:
            fsck = subprocess.run(["fsck.vfat", "-n", dev], capture_output=True, text=True)
            check("fsck.vfat -n clean after the kernel's own writes", fsck.returncode == 0, fsck.stdout.strip().splitlines()[-1] if fsck.stdout.strip() else "")


crashed = None
try:
    main()
except BaseException as exc:  # a crash is a failure, not a pass of whatever ran before it
    crashed = exc
    check("the test ran to the end without an exception", False, f"{type(exc).__name__}: {exc}")
finally:
    ok = all(r["ok"] for r in results) and bool(results) and crashed is None
    skipped = sum(1 for r in results if r.get("skipped"))
    print("RESULT:", "ALL PASS" if ok else "FAILURES", f"({sum(r['ok'] and not r.get('skipped') for r in results)} passed, {skipped} skipped, {len(results)} total)")
    json.dump(results, open(sys.argv[1] if len(sys.argv) > 1 else "/dev/null", "w"), indent=1)
if crashed is not None or not ok:
    sys.exit(1)
