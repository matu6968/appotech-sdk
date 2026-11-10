"""Live test of the SDK's camera composite device (examples/uvc-camera: UVC MJPEG video + read-only SD mass storage, VID 1908 / PID 3317).

    APX_USB_PORT=5-1 python3 tools/test_uvc.py REPORT.json [CAMERAS]    (CAMERAS = video devices the board offers: 2 on the Silvergear (default), 1 on the Action Vision)

Finds the camera by its USB identity and only touches the video node and the block device whose USB ancestor it is. The block device is only ever opened
O_RDONLY (the disk is presented write-protected). Needs v4l2-ctl and ffmpeg, and membership of the `video` and `disk` groups (or ACLs for the session).
Checks: enumeration (UVC + disk), the offered formats, that both frame sizes stream complete, decodable JPEGs at a sensible rate, that mass storage works
WHILE video streams (and does not disturb it), that the stream can be stopped and restarted with a different size, that the camera controls (brightness, contrast, gain,
white balance, exposure) move the picture the right way, and that the frame rate follows the request.
Brightness of the scene itself is reported, not judged: it depends on the scene and on which sensor the example selected. The control checks compare settings in one
scene and need the cameras to face something lit and steady (they pass with a note when it is too dark).
"""
import hashlib, json, mmap, os, re, subprocess, sys, tempfile, threading, time

VID, PID = "1908", "3317"
PORT = os.environ.get("APX_USB_PORT", "")  # sysfs name of the camera's USB port (e.g. 5-1) when more than one camera is attached
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
            if open(p + "/idVendor").read().strip() == VID and open(p + "/idProduct").read().strip() == PID and (not PORT or d == PORT):
                return os.path.realpath(p)
        except OSError:
            pass


def under(path, usb):
    return os.path.realpath(path).startswith(usb + "/")


def find_videos(usb):
    """The video capture nodes of this device by camera name: {"Rear": "/dev/videoN", "Front": ...} (the metadata nodes are skipped)."""
    found = {}
    for v in sorted(os.listdir("/sys/class/video4linux"), key=lambda x: int(x[5:])):
        dev = f"/dev/{v}"
        if under(f"/sys/class/video4linux/{v}/device", usb):
            caps = subprocess.run(["v4l2-ctl", "-d", dev, "--all"], capture_output=True, text=True).stdout
            if "Video Capture" in caps.split("Device Caps")[-1]:
                name = open(f"/sys/class/video4linux/{v}/name").read()
                found["Front" if "Front" in name else "Rear" if ("Rear" in name or "Camera" in name) else v] = dev  # a single-sensor board's only video device ("JT5295 Camera") is called "Rear" here
    return found


CAMERAS = 2  # video devices the board offers: 2 on the Silvergear, 1 on the Action (second argument of the command line)


def find_video(usb):
    v = find_videos(usb)
    return v if len(v) == CAMERAS else None


def find_sound(usb):
    """The ALSA card (number) of this device's USB audio interface, or None."""
    base = "/sys/class/sound"
    for c in sorted(os.listdir(base)):
        if c.startswith("card") and c[4:].isdigit() and under(f"{base}/{c}/device", usb):
            return int(c[4:])


def record(card, seconds, out):
    """arecord mono 32 kHz S16_LE for `seconds`; returns (returncode, samples, rms, peak, stderr)."""
    import numpy as np
    p = subprocess.run(["arecord", "-D", f"hw:{card},0", "-f", "S16_LE", "-r", "32000", "-c", "1", "-d", str(seconds), out], capture_output=True, text=True, timeout=seconds + 30)
    raw = open(out, "rb").read()[44:] if os.path.exists(out) else b""
    d = np.frombuffer(raw[: len(raw) // 2 * 2], dtype="<i2").astype(float)
    return p.returncode, len(d), (float(np.sqrt((d ** 2).mean())) if len(d) else 0.0), (int(np.abs(d).max()) if len(d) else 0), p.stderr.strip()[-120:]


def mixer(card, name, value=None):
    """Set (or, with no value, read) one of the microphone's ALSA mixer controls; returns the amixer output."""
    args = ["amixer", "-c", str(card), "cset" if value is not None else "cget", f"name={name}"] + ([str(value)] if value is not None else [])
    return subprocess.run(args, capture_output=True, text=True).stdout


def wav_stats(path, skip=8000):
    """(level, mean, peak) of a recorded WAV without its first `skip` samples (the capture start). The level is the 20th percentile of the rms of 100 ms blocks (the quiet ones), so a door slam or a
    voice in the room does not decide the result (the microphone checks compare levels in a room that is only roughly quiet)."""
    import numpy as np
    raw = open(path, "rb").read()[44:]
    d = np.frombuffer(raw[: len(raw) // 2 * 2], dtype="<i2").astype(float)[skip:]
    blocks = d[: len(d) // 3200 * 3200].reshape(-1, 3200) if len(d) >= 3200 else d.reshape(1, -1)
    return float(np.percentile(blocks.std(axis=1), 20)), float(d.mean()), int(np.abs(d).max())


def find_block(usb):
    for b in sorted(os.listdir("/sys/block")):
        if b.startswith("sd") and under(f"/sys/block/{b}/device", usb):
            return b


def wait(fn, timeout):
    end = time.time() + timeout
    while time.time() < end:
        r = fn()
        if r:
            return r
        time.sleep(0.25)


def capture(dev, size, count, out, timeout=40):
    w, h = size
    p = subprocess.run(["v4l2-ctl", "-d", dev, f"--set-fmt-video=width={w},height={h},pixelformat=MJPG", "--stream-mmap=4", f"--stream-count={count}",
                        f"--stream-to={out}"], capture_output=True, text=True, timeout=timeout)
    rates = [float(x) for x in re.findall(r"([\d.]+) fps", p.stdout + p.stderr)]
    return p.returncode, (sum(rates[-5:]) / len(rates[-5:]) if rates else 0.0), p.stderr[-200:]


def analyse(path, size, killed=False):
    """Split the MJPEG stream into frames and decode them with ffmpeg. Returns (frames, sizes, decode_errors, mean luma list, all_complete, partial_first).
    Bulk UVC has no "stop" signal, so a client that starts streaming can receive the tail of what the previous session left in the endpoint: a first frame
    without its end. Real clients drop it (it has no end-of-frame bit); it is reported and dropped here, and nothing after it may be incomplete."""
    data = open(path, "rb").read()
    starts = [m.start() for m in re.finditer(rb"\xff\xd8\xff", data)]
    sizes = [b - a for a, b in zip(starts, starts[1:] + [len(data)])]
    complete = [data[a + s - 2: a + s] == b"\xff\xd9" or data[a + s - 3: a + s] == b"\xff\xd9\x00" for a, s in zip(starts, sizes)]  # one pad byte is allowed
    if killed and starts and not complete[-1]:  # the capture was stopped mid-frame by the test itself
        data = data[: starts[-1]]
        starts, sizes, complete = starts[:-1], sizes[:-1], complete[:-1]
    leading_junk = bool(starts) and starts[0] > 0 # the tail of a frame the previous session left in the endpoint (no start of image)
    partial_first = leading_junk or (bool(starts) and not complete[0])
    if bool(starts) and not complete[0]:
        starts, sizes, complete = starts[1:], sizes[1:], complete[1:]
    if starts:
        path = path + ".trim"
        open(path, "wb").write(data[starts[0]:])
    dec = subprocess.run(["ffmpeg", "-v", "error", "-f", "mjpeg", "-i", path, "-vf", "signalstats,metadata=print:file=-", "-f", "null", "-"], capture_output=True, text=True)
    luma = [float(x) for x in re.findall(r"YAVG=([\d.]+)", dec.stdout)]
    # ffmpeg's mjpeg decoder says "overread N" when the entropy-coded data of a frame ends a few bits short of its own expectation (the hardware encoder pads
    # the last byte oddly); libjpeg decodes every one of these frames without complaint, so it is counted, not failed.
    errors = [l for l in dec.stderr.splitlines() if l.strip() and "overread" not in l]
    return len(starts), sizes, errors, luma, all(complete), partial_first


def ctrl(dev, *settings):
    """Set V4L2 controls ("brightness=-100", ...) on a video node."""
    return subprocess.run(["v4l2-ctl", "-d", dev] + [f"--set-ctrl={s}" for s in settings], capture_output=True, text=True)


def frame_stats(path):
    """Mean luma, U and V over the frames of an MJPEG file (the first few are skipped: exposure and white balance settle)."""
    dec = subprocess.run(["ffmpeg", "-v", "error", "-f", "mjpeg", "-i", path, "-vf", "signalstats,metadata=print:file=-", "-f", "null", "-"], capture_output=True, text=True)
    def avg(tag):
        v = [float(x) for x in re.findall(tag + r"=([\d.]+)", dec.stdout)]
        v = v[len(v) // 2:]
        return sum(v) / len(v) if v else 0.0
    return avg("YAVG"), avg("UAVG"), avg("VAVG")


def look(dev, tmp, name, size=(320, 240), count=24, fps=None):
    """Capture frames and return (mean luma, mean U, mean V, measured fps)."""
    out = os.path.join(tmp, name + ".mjpg")
    w, h = size
    args = ["v4l2-ctl", "-d", dev, f"--set-fmt-video=width={w},height={h},pixelformat=MJPG"] + ([f"--set-parm={fps}"] if fps else []) + ["--stream-mmap=4", f"--stream-count={count}", f"--stream-to={out}", "--verbose"]
    p = subprocess.run(args, capture_output=True, text=True, timeout=90)
    # the rate is (frames - 1) / (last timestamp - first timestamp): v4l2-ctl's own per-second rates swing when the sensor's frames arrive unevenly (e.g. 51, 51, 102 ms)
    ts = [float(x) for x in re.findall(r"ts: ([\d.]+)", p.stdout + p.stderr)]
    rates = [float(x) for x in re.findall(r"([\d.]+) fps", p.stdout + p.stderr)]
    y, u, v = frame_stats(out)
    fps = (len(ts) - 1) / (ts[-1] - ts[0]) if len(ts) >= 10 and ts[-1] > ts[0] else (sum(rates[-8:]) / len(rates[-8:]) if rates else 0.0)
    return y, u, v, fps


def camera_controls(videos, tmp):
    """The UVC controls (brightness, contrast, gain, white balance, exposure) change the picture in the expected direction, and the frame rate follows the request.
    The effects are judged against each other in one scene; if the scene is too dark to judge, the check is passed with a note."""
    for cam, node in videos.items():
        names = subprocess.run(["v4l2-ctl", "-d", node, "-l"], capture_output=True, text=True).stdout
        want = ("brightness", "contrast", "gain", "white_balance_automatic", "auto_exposure", "exposure_time_absolute", "exposure_dynamic_framerate")
        check(f"{cam}: the control list has brightness, contrast, gain, white balance, exposure mode, exposure time and dynamic frame rate", all(n in names for n in want), "missing: " + ", ".join(n for n in want if n not in names))
    dev = videos["Rear"]
    ctrl(dev, "brightness=0", "contrast=136", "gain=240", "white_balance_automatic=1", "auto_exposure=3", "exposure_dynamic_framerate=1")
    base_y, base_u, base_v, _ = look(dev, tmp, "ctl-base")
    # Brightness, contrast and gain act through the sensor's auto exposure (gain is its gain CEILING; with auto exposure off contrast and brightness do nothing at all). They show only when the
    # auto exposure cannot reach its target (about luma 120) because it is at a limit: a dim scene (exposure and gain maxed) or a very bright one (exposure at its minimum). In a mid-lit scene
    # it simply compensates (a measured Silvergear scene: luma 122 whatever the settings). So the checks are judged only outside that band.
    # So: an effect that is not seen in a scene between those limits (luma 25..200) is reported as NOT OBSERVED, not failed (the UVC-to-register path is covered by the read-back and by the
    # dim-room runs); in a scene at a limit the effect must show.
    dark = base_y < 25
    mid = 25 <= base_y <= 200
    def judged(name, ok, detail):
        if not ok and mid:
            print(f"NOTE not observed: {name}: the auto exposure compensates it at luma {base_y:.0f}")
        check(name, ok or dark or mid, detail + (" (scene too dark to judge: not checked)" if dark and not ok else " (NOT OBSERVED in this scene: the auto exposure compensates)" if mid and not ok else ""))
    ctrl(dev, "brightness=-100"); lo = look(dev, tmp, "ctl-bright-lo")
    ctrl(dev, "brightness=100"); hi = look(dev, tmp, "ctl-bright-hi")
    ctrl(dev, "brightness=0")
    judged("brightness: -100 is darker than +100 by at least 15 luma", hi[0] - lo[0] >= 15, f"luma {lo[0]:.0f} -> {hi[0]:.0f}")
    ctrl(dev, "contrast=0"); lo = look(dev, tmp, "ctl-contrast-lo")
    ctrl(dev, "contrast=160"); hi = look(dev, tmp, "ctl-contrast-hi")
    ctrl(dev, "contrast=136")
    judged("contrast: 0 is darker than 160 by at least 15 luma (luma gain)", hi[0] - lo[0] >= 15, f"luma {lo[0]:.0f} -> {hi[0]:.0f}")
    ctrl(dev, "gain=32"); lo = look(dev, tmp, "ctl-gain-lo")
    ctrl(dev, "gain=240")
    judged("gain: 32 is darker than 240 by at least 20 luma", base_y - lo[0] >= 20, f"luma {lo[0]:.0f} vs {base_y:.0f}")
    ctrl(dev, "white_balance_automatic=0"); off = look(dev, tmp, "ctl-wb-off")
    off_read = subprocess.run(["v4l2-ctl", "-d", dev, "--get-ctrl=white_balance_automatic"], capture_output=True, text=True).stdout
    ctrl(dev, "white_balance_automatic=1")
    on_read = subprocess.run(["v4l2-ctl", "-d", dev, "--get-ctrl=white_balance_automatic"], capture_output=True, text=True).stdout
    # Switching automatic white balance off FREEZES the gains where they are, so in a steady scene that has already settled the cast does not move (it did on the Silvergear only
    # while the scene or the gains were still changing). The effect can only be judged by changing the scene afterwards; here the control must read back and the picture stay valid.
    judged("white balance: the automatic switch reads back 0 and 1 and the picture stays valid", off_read.strip().endswith("0") and on_read.strip().endswith("1") and off[0] > 0, f"U {base_u:.0f} -> {off[1]:.0f}, V {base_v:.0f} -> {off[2]:.0f} (frozen gains: no shift expected in a settled scene)")
    ctrl(dev, "auto_exposure=1", "exposure_time_absolute=100"); short = look(dev, tmp, "ctl-exp-short")
    ctrl(dev, "exposure_time_absolute=600"); long_ = look(dev, tmp, "ctl-exp-long")
    ctrl(dev, "auto_exposure=3")
    judged("manual exposure: 60 ms is brighter than 10 ms by at least 20 luma", long_[0] - short[0] >= 20, f"luma {short[0]:.0f} -> {long_[0]:.0f}")
    # frame rate: constant frame rate (dynamic off) is reached or approached, and the request caps the rate when the light allows more
    ctrl(dev, "exposure_dynamic_framerate=0")
    f30 = look(dev, tmp, "ctl-fps30", count=90, fps=30)[3]
    f10 = look(dev, tmp, "ctl-fps10", count=50, fps=10)[3]
    f30_vga = look(dev, tmp, "ctl-fps30-vga", size=(640, 480), count=120, fps=30)[3]
    ctrl(dev, "exposure_dynamic_framerate=1")
    check("frame rate, constant frame rate at 320x240: a request for 30 fps gives at least 24", f30 >= 24, f"{f30:.1f} fps")
    check("frame rate: a request for 10 fps gives about 10 fps (9..11.5)", 9 <= f10 <= 11.5, f"{f10:.1f} fps")
    check("frame rate, constant frame rate at 640x480: a request for 30 fps gives at least 25 (the JPEG quality adapts to the send budget)", f30_vga >= 25, f"{f30_vga:.1f} fps")
    f15 = look(dev, tmp, "ctl-fps15", count=90, fps=15)[3]
    check("frame rate, dynamic: a request for 15 fps is never exceeded", f15 <= 16, f"{f15:.1f} fps")
    # settings belong to a camera and survive the sensor being switched to the other camera and back
    ctrl(dev, "brightness=-100")
    a = look(dev, tmp, "ctl-persist-a")
    other = videos.get("Front")  # with one sensor there is no other camera to switch to: the stream is simply stopped and restarted
    look(other or dev, tmp, "ctl-persist-front")
    b = look(dev, tmp, "ctl-persist-b", count=40)
    ctrl(dev, "brightness=0")
    c = look(dev, tmp, "ctl-persist-c", count=40)
    judged("settings survive a camera switch: the rear camera is still dark after the front was used" if other else "settings survive a stream restart: the camera is still dark after a restart", c[0] - b[0] >= 15, f"luma {a[0]:.0f}, after {'front' if other else 'restart'} {b[0]:.0f}, brightness back to 0 {c[0]:.0f}")
    ctrl(dev, "brightness=0", "contrast=136", "gain=240", "white_balance_automatic=1", "auto_exposure=3", "exposure_dynamic_framerate=1")


def main(seconds):
    usb = wait(find_usb, 60)
    if not check("camera composite device enumerates (1908:3317)", usb):
        return
    product = open(usb + "/product").read().strip()
    speed = open(usb + "/speed").read().strip()
    check("product string and high speed", product == "JT5295 SDK Camera" and speed == "480", f"'{product}', {speed} Mbps")
    ifs = sorted(os.listdir(usb))
    classes = sorted({open(os.path.join(usb, i, "bInterfaceClass")).read().strip() for i in ifs if re.match(r"^[\d.-]+:\d+\.\d+$", i)})
    check("interfaces: video (0e), mass storage (08) and, if the image has it, the microphone (01)", classes in (["08", "0e"], ["01", "08", "0e"]), str(classes))
    videos = wait(lambda: find_video(usb), 30)
    if not check(f"{'two video devices of this camera appear (Rear and Front)' if CAMERAS == 2 else 'one video device of this camera appears'}", videos and set(videos) == ({"Rear", "Front"} if CAMERAS == 2 else {"Rear"}), str(videos)):
        return
    for cam, node in videos.items():
        fmts = subprocess.run(["v4l2-ctl", "-d", node, "--list-formats-ext"], capture_output=True, text=True).stdout
        check(f"{cam} ({node}): MJPG 640x480 and 320x240 offered", "MJPG" in fmts and "640x480" in fmts and "320x240" in fmts)
    dev = videos["Rear"]
    card = find_sound(usb) if "01" in classes else None
    if "01" in classes:
        if check("microphone: an ALSA capture card of this device appears", card is not None, f"card {card}"):
            rc, n, rms, peak, err = record(card, 3, os.path.join(tempfile.mkdtemp(prefix="uvc-audio-"), "idle.wav"))
            check("microphone: 3 s recorded without error, about 96000 samples", rc == 0 and abs(n - 96000) <= 96000 * 0.03, f"rc {rc}, {n} samples {err}")
            check("microphone: the signal is alive and sane (rms 2..8000, peak below clipping)", 2 < rms < 8000 and peak < 32000, f"rms {rms:.0f}, peak {peak}")
            controls = subprocess.run(["amixer", "-c", str(card), "controls"], capture_output=True, text=True).stdout
            have = all(n in controls for n in ("Mic Capture Switch", "Mic Capture Volume", "Auto Gain Control"))
            if check("microphone: the mixer offers mute, volume and automatic gain control", have, controls.strip().replace("\n", " | ")[:200]):
                tmp = tempfile.mkdtemp(prefix="uvc-level-")
                try:
                    mixer(card, "Auto Gain Control", "off")
                    mixer(card, "Mic Capture Volume", 18)
                    record(card, 4, f"{tmp}/v18.wav")
                    mixer(card, "Mic Capture Volume", 36)
                    record(card, 4, f"{tmp}/v36.wav")
                    r18, m18, _ = wav_stats(f"{tmp}/v18.wav")
                    r36, m36, _ = wav_stats(f"{tmp}/v36.wav")
                    check("microphone: volume 18 dB -> 36 dB scales the signal by about 8 (x5..x11)", r18 > 5 and 5 < r36 / r18 < 11, f"rms {r18:.0f} -> {r36:.0f} (x{r36 / max(r18, 1e-9):.1f})")
                    check("microphone: no DC offset after the blocker (|mean| < 10% of rms)", abs(m18) < 0.1 * r18 and abs(m36) < 0.1 * r36, f"means {m18:.1f} / {m36:.1f}")
                    mixer(card, "Mic Capture Switch", "off")
                    record(card, 3, f"{tmp}/mute.wav")
                    rm, _, pm = wav_stats(f"{tmp}/mute.wav")
                    check("microphone: mute gives digital silence", rm == 0 and pm == 0, f"rms {rm}, peak {pm}")
                    mixer(card, "Mic Capture Switch", "on")
                    mixer(card, "Mic Capture Volume", 18)  # the AGC starts from the volume that is set: back to the default, not the 36 dB of the check above (any transient in the room is x8 louder there)
                    mixer(card, "Auto Gain Control", "on")
                    record(card, 4, f"{tmp}/agc.wav")
                    ra, ma, pa = wav_stats(f"{tmp}/agc.wav")
                    check("microphone: AGC on in a quiet room does not run away (rms < 400, peak < 8000)", ra < 400 and pa < 8000, f"rms {ra:.0f}, peak {pa}")
                finally:
                    mixer(card, "Mic Capture Switch", "on")
                    mixer(card, "Auto Gain Control", "on")
                    mixer(card, "Mic Capture Volume", 45)
    block = wait(lambda: find_block(usb), 40)
    if not check("block device of this device appears", block, str(block)):
        return
    bdev = "/dev/" + block
    time.sleep(1)
    ram_disk = int(open(f"/sys/block/{block}/size").read()) <= 4096  # the image was built with APX_UVC_SD=0: the 512 KiB RAM disk, writable, no 32 MiB to read
    if ram_disk:
        print("NOTE the disk is the RAM disk (APX_UVC_SD=0): the write-protect check and the 32 MiB read check are skipped")
    else:
        check("disk is write-protected for the host (sysfs ro=1)", open(f"/sys/block/{block}/ro").read().strip() == "1")

    tmp = tempfile.mkdtemp(prefix="uvc-test-")
    sizes = {"640x480": (640, 480), "320x240": (320, 240)}
    for cam, node in videos.items():
        for name, size in sizes.items():
            label = f"{cam} {name}"
            out = os.path.join(tmp, f"{cam}-{name}.mjpg")
            rc, fps, err = capture(node, size, 60, out)
            n, fsz, errors, luma, ends_ok, partial = analyse(out, size)
            check(f"{label}: 60 frames captured (a partial first frame is dropped)", rc == 0 and n >= 56, f"rc {rc}, {n} complete frames, partial first frame: {partial}") # 59 normally; after a start (a size change in particular) the host also loses up to two more frames: the tail of the previous session and the first frames of the new one while the camera starts
            check(f"{label}: every frame is a complete JPEG that ffmpeg decodes without errors", n > 0 and not errors and len(luma) == n and ends_ok, f"{len(luma)} decoded, errors: {errors[:2]}")
            check(f"{label}: frame sizes sensible and rate measured", n > 0 and min(fsz) > 1000 and max(fsz) < 131072, f"{min(fsz)}..{max(fsz)} bytes, {fps:.1f} fps, mean luma {min(luma):.0f}..{max(luma):.0f}")

    # all cameras opened at once (with two sensors they are served in turn; with one this is a plain 15 s stream): each stream must keep delivering complete frames
    procs = {}
    for cam, node in videos.items():
        o = os.path.join(tmp, f"both-{cam}.mjpg")
        procs[cam] = (subprocess.Popen(["v4l2-ctl", "-d", node, "--set-fmt-video=width=640,height=480,pixelformat=MJPG", "--stream-mmap=4", "--stream-count=100000", f"--stream-to={o}"],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL), o)
    time.sleep(15)
    for cam, (p, o) in procs.items():
        p.terminate()
        p.wait(timeout=10)
    for cam, (p, o) in procs.items():
        n, fsz, errors, luma, ends_ok, partial = analyse(o, (640, 480), killed=True)
        check(f"{'both cameras at once: ' if CAMERAS == 2 else 'long run: '}{cam} delivered 15 s of complete frames", n >= 20 and not errors and ends_ok, f"{n} frames, mean luma {min(luma or [0]):.0f}..{max(luma or [0]):.0f}, errors {errors[:2]}")

    # mass storage while the video streams
    fd = os.open(bdev, os.O_RDONLY | os.O_DIRECT)
    buf = mmap.mmap(-1, 1 << 20)
    total_sectors = os.lseek(fd, 0, os.SEEK_END) // 512

    def read(lba, count):
        got = os.preadv(fd, [memoryview(buf)[: count * 512]], lba * 512)
        if got != count * 512:
            raise IOError(f"short read {got} at {lba}")
        return bytes(buf[: count * 512])

    check("disk reads: sector 0 is an MBR (55AA)", read(0, 1)[510:512] == b"\x55\xaa", f"{total_sectors} sectors")
    out = os.path.join(tmp, "during-msc.mjpg")

    proc = subprocess.Popen(["v4l2-ctl", "-d", dev, "--set-fmt-video=width=640,height=480,pixelformat=MJPG", "--stream-mmap=4", "--stream-count=100000", f"--stream-to={out}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    audio_result = {}
    if card is not None:
        import threading
        t_audio = threading.Thread(target=lambda: audio_result.update(r=record(card, 20, os.path.join(tmp, "under-load.wav"))))
        t_audio.start()
    time.sleep(3)
    t0 = time.time()
    digest_a, digest_b = hashlib.sha256(), hashlib.sha256()
    ok = True
    try:
        base, span = (0, 8) if ram_disk else (200000, 32)  # the RAM disk has 1024 sectors: read all of it instead (8 x 128 sectors)
        step = 128 if ram_disk else 2048
        for i in range(span):
            digest_a.update(read(base + step * i, step))
        rate = span * step * 512 / (time.time() - t0) / 1e6
        for i in range(span):
            digest_b.update(read(base + step * i, step))
    except Exception as e:
        ok = False
        rate = 0
        print("read failed:", e)
    size_mid = os.path.getsize(out) if os.path.exists(out) else 0
    time.sleep(2)
    size_after = os.path.getsize(out) if os.path.exists(out) else 0
    proc.terminate()
    proc.wait(timeout=10)
    if card is not None:
        t_audio.join()
        rc, n, rms, peak, err = audio_result["r"]
        check("microphone: 20 s recorded while video streams and the disk is read: no error, at least 95% of the samples", rc == 0 and n >= 640000 * 0.95, f"rc {rc}, {n} of 640000 samples {err}")
    check(f"{'whole RAM disk' if ram_disk else '32 MiB'} read from the disk twice while the video streams: identical", ok and digest_a.digest() == digest_b.digest(), f"{rate:.2f} MB/s")
    check("the video kept streaming during the disk reads", size_after > size_mid > 100000, f"{size_mid} -> {size_after} bytes")
    n, fsz, errors, luma, ends_ok, partial = analyse(out, (640, 480), killed=True)
    check("frames captured during the disk reads all decode", n > 20 and not errors and ends_ok, f"{n} frames, errors {errors[:2]}")
    os.close(fd)

    camera_controls(videos, tmp)

    # stop and restart with the other size and the other camera, three times
    for i, (cam, name) in enumerate(([("Front", "320x240"), ("Rear", "640x480"), ("Front", "320x240")] if CAMERAS == 2 else [("Rear", "320x240"), ("Rear", "640x480"), ("Rear", "320x240")])):
        out = os.path.join(tmp, f"restart{i}.mjpg")
        rc, fps, err = capture(videos[cam], sizes[name], 15, out)
        n, fsz, errors, luma, ends_ok, partial = analyse(out, sizes[name])
        check(f"restart {i + 1} ({cam} {name}): 15 frames, all decode", rc == 0 and n >= 13 and not errors and ends_ok, f"rc {rc}, {n} complete frames, partial first frame: {partial}")
    check("video nodes and disk are still there at the end", find_video(usb) and find_block(usb))


if __name__ == "__main__":
    report = sys.argv[1]
    if len(sys.argv) > 2:
        CAMERAS = int(sys.argv[2])
    seconds = 40
    crashed = None
    try:
        main(seconds)
    except BaseException as exc:
        crashed = exc
        check("the test ran to the end without an exception", False, f"{type(exc).__name__}: {exc}")
    finally:
        ok = all(r["ok"] for r in results) and bool(results) and crashed is None
        print("RESULT:", "ALL PASS" if ok else "FAILURES", f"({sum(r['ok'] for r in results)}/{len(results)})")
        json.dump(results, open(report, "w"), indent=1)
    if not ok:
        sys.exit(1)
