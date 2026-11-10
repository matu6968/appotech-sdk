# Action Vision compatibility pass (2026-11-09)

The TinyUSB stack, the SD rewrite (CMD18/CMD25 streaming, high speed), the camera controls, the JPEG quality scale, the microphone continuous mode and the relocated buffers were all
developed on the Silvergear. This pass ran them on the Action Vision camera (`board: action-vision-jt5295`, one sensor ID 0x39, no printer, 220x176 LCD, an inherited ~96 MHz CPU
estimate), **RAM launches only, nothing written to SPI**. The owner approved SD writes on the card in the camera within the usual footprint (all-zero MBR-gap sectors, saved and
restored, plus one scratch file that is deleted); a before/after read of sectors 0..631 showed one differing sector (539, a single 16-byte directory-entry slot: the scratch file's own
deleted entry replacing a leftover one) and nothing else.

Launching from the stock firmware needs `--reclaim-low-ram --reclaim-jpeg-ram` (the stock firmware leaves data in both windows; apxtool saves it aside). The Action's build directory is
`build/` (the legacy-stack variant `build-legacy/`), not `build/<board>/`. **TinyUSB has been the Action's default stack since 2026-11-10** (before that it was opt-in with `USB_STACK=tinyusb` and
built into `build-tinyusb/`; the results below say which stack they used). No TinyUSB image has been cold-booted from SPI on the Action yet.

| Area | Result on the Action |
| --- | --- |
| Debug link, legacy stack | `tools/test_usb.py` 5/5, 480 Mbps, ~10,000 soak cycles per 30 s |
| Debug link, **TinyUSB** (packet RAM, polled DCD) | 5/5; 12/12 then 40/40 fresh launches after the fix below (**found a bug**, below) |
| Mass storage, RAM disk, TinyUSB | `tools/test_usb_msc.py` 20/20, 3.15 / 3.10 MB/s read / write (the same as the Silvergear: the packet RAM path works on this core) |
| SD card (SanDisk SU02G 2 GB SDSC) | driver 48 MHz, high speed; `sd-bench`: 5.6 MB/s sequential, 5.5 MB/s in 8-sector calls, 0.97 MB/s random, 0 errors, every sector equal to the raw reference, last 8 sectors of the card fine |
| SD write path (`sd-card-test`) | 8/8 steps, 0 errors, 0 bad data-response tokens; file write 1.2-1.8 MB/s, read-back 2.1 MB/s |
| SD as a USB disk, read-write (`tools/test_usb_msc_sd_rw.py`) | 18/18, 16 MiB file written 1.48 MB/s, read back 1.99 MB/s with the same SHA-256 |
| Sensor (ID 0x39) register access, frame timing | same as the Silvergear: 80.4 ms frames at the stock exposure ceiling, 30.03 fps at a cap of 3 (29.3 fps with 12 blank lines); AE reacts to a flashlight (exposure 1208 -> 302 lines) |
| `camera-demo`: raw NV21 frame, hardware JPEG at 320x240 / 640x480 / 1280x960 | frames complete and decode at the right sizes (luma 127 in a lit scene); LCD live preview confirmed right by eye |
| JPEG encode quality scale | works (quality 90 = stock size, 50 about half), valid files |
| Microphone, continuous capture + prepared re-arm (`mic-probe` experiment 4) | 33,554 ticks per 32,768-sample buffer = 32,000 Hz, handover latency 2 ticks (max 3), no sentinel samples left |
| Display (`display-demo`) | 220x176, frame buffer in the relocated low window (0x02080000), 0 timeouts, ~78 fps native NV21 and ~15 fps RGB565 DMA stress |
| LVGL demo | ran through to the results screen (by eye) |
| Speaker | 1 s 880 Hz tone, clean (by ear) |
| Build | every example builds for the Action with both stacks except the known board-specific ones (camera-demo on Silvergear; printer-sd-* on the Action) |

Not applicable on the Action: the printer examples. (UVC was added afterwards as a single-camera device, see the end of this file.) Not run: the live tools that save files to the card (`test_jpeg_live`, `test_audio_live`,
`test_microphone_live`: outside the approved footprint), and the hot-pull tests (need hands on the card).

## The bug this found: an EP1 deadlock that only the Action's timing exposed

The video-speed work made the DCD queue **two** packets per refill and made IN completion wait for the core's FIFO to drain. On the Silvergear everything passed. On the Action
`test_usb.py` failed in 5-20% of fresh launches (the same 4 checks passed, then "short USB write" in the sustained loop; 1 of 20 even with the two-packet fill switched off, which pointed
at the drain wait). A temporary DCD event log plus a class-request register snapshot of the stuck endpoint (vendor-type control requests do not reach class drivers; a class request to
the interface does) showed: `TXCSRH` still in TX mode, TX FIFO not empty, IN pipe armed, OUT pipe not armed, and RXCSRL = 3 with a 512-byte request packet waiting.
EP1's IN and OUT share one FIFO. The host had already sent its next request, whose first packet sat in that FIFO and kept FIFO-not-empty set; the debug code only consumes a request
after the IN transfer completes, and the IN transfer could not complete until the FIFO emptied. The Action's slower CPU polls late enough for the host to win the race; the Silvergear's does not.

Fix (`dcd_apx.c`): the two-packet fill and the drain wait apply to EP2 and up only (no OUT traffic there); EP1 is back to the original single packet and no wait (it gained nothing from them).
40 of 40 fresh launches pass afterwards (12 of 12 on the final clean build). The same race was possible on the Silvergear with different timing.

## Demo pass on the Action (2026-11-09/10)

All remaining demos ran on the Action Vision with the user doing the physical parts: muldiv-test, the probes, sd-write-test (22/22 on a run with no USB access during the test),
camera/LCD preview (live picture correct once the lens was uncovered; an apparently black camera was a dark lens, proven with the sensor-probe luma and exposure under a flashlight),
LVGL (ran to the results screen), speaker tone, microphone round trip, JPEG on the LCD, card audio, LED, firmware-side card hot-pull and USB-side card hot-pull.

Two things to know:

- **Reading the APXD log in the middle of sd-write-test wedged the debug link** (the device kept answering a stale LOG reply with sequence 2) and needed a power cycle; the card was
  checked afterwards (gap sectors zero, no scratch file left). Not investigated; do not read the log while a long test runs.
- **Card incident.** After the USB-side hot-pull test the card's FAT had entries 7 and 14 (the single clusters of the PHOTO and SDKJPEG directories) at 0x0000 in both FAT copies; every
  snapshot taken before that test had 0xFFFF, and nothing else in sectors 0..631 had changed. The host kernel automounted the card several times during that test (read errors at
  each pull, boot-sector dirty flag already set before it). **The writer was not identified**: none of the SDK tests writes outside the MBR gap, but the host mount or the device cannot be
  excluded. A later `fsck.vfat` (run by the user) deleted the two directory entries and made 69 `FSCKnnnn.REC` files from the orphaned chains. No data was lost (directory clusters and
  JPEG data intact); with approval the two FAT entries and root-directory sectors 536..543 were written back from the pre-fsck snapshot, and the folders listed correctly again. Leftover
  difference: the boot sector's dirty flag is cleared and its label field says "APX SDK" (fsck).
  Lesson for the hot-pull tests: unplug the camera's USB data path from the desktop automounter (or mount the card read-only on the host) when the card is pulled during a USB MSC test.

## UVC on the Action: one video function (2026-11-10)

Most webcams have one sensor, so the camera composite device now has **one video function per sensor of the board** (`APX_CAMERA_COUNT`, which `usb_uvc.h` calls `APX_UVC_STREAMS`): the
Silvergear is unchanged (two functions, interfaces 0..3, mass storage 4, microphone 5/6); the Action shows one video device "JT5295 Camera" (interfaces 0/1, EP2), mass storage as
interface 2 and the microphone 3/4. `examples/uvc-camera` no longer refuses to build on the Action; `tools/test_uvc.py REPORT.json 1` tests the one-camera layout.

Result on the Action Vision (TinyUSB image, RAM disk instead of the card so the host never mounts the user's card; build `APX_UVC_SD=0`): **41/41 checks, twice**: enumeration, both frame sizes
decode, 15 s long run, microphone checks, all camera controls in a lit scene, 28.9 fps at 30 fps requested (320x240 and 640x480), 10.6 at 10, restarts, disk reads while streaming.
Not yet run on the Action: the SD card as the disk (needs the host to mount the card read-only; given the unexplained FAT incident above it is only done with the user's go-ahead).

Findings from the first runs (the first run in a dim room failed 7 of 42):
- Three of the failures were the RAM disk (writable, 1024 sectors); the test now recognises it and skips the write-protect and 32 MiB checks.
- The contrast / manual-exposure / persistence checks need a lit scene (luma ~6..30 in the dim room).
- **White balance:** switching automatic off FREEZES the gains, so in a settled scene the colour cast does not move. The old check passed on the Silvergear only while the scene was changing. The
  test now checks that the control reads back; the real effect needs a scene change after switching it off (not automated).
- **Frame pacing bug in `examples/uvc-camera`:** the due time was only reset when the stream was more than 4 frame intervals behind, so a stream restarted a little after the last frame ran at the
  sensor's rate (up to 19.6 fps for a 15 fps request) until the lag was used up. It now resets when more than one interval behind. The test measures the rate from frame timestamps (v4l2-ctl's
  per-second rates swing when frames arrive as 51/51/102 ms).
- Frames in a lit scene arrive unevenly (the sensor frame is 51 ms, the pacing keeps the mean at the request): 15.8 fps measured for a request of 15 over 45 frames, under the test's limit of 16.

## SD card as the disk on the Action; Silvergear regression; the idle-detection bug (2026-11-10)

**SD card on the Action** (`examples/uvc-camera`, TinyUSB, card = the 2 GB "APX SDK" card): the kernel reported "Write Protect is on", the host never mounted it, sector 0 reads as an MBR, 32 MiB read
twice during video are identical (1.2 MB/s with video streaming). Sectors 0..631 of the card (boot sector, both FATs, root directory) are byte-identical before and after (read-only snapshots with sd-demo). The one
test difference that is not the disk: brightness/contrast/gain were not observable in that scene (see below).

**Silvergear** (final DCD, port-pinned launcher): 50/50 with `tools/test_uvc.py REPORT 2` (RAM disk: the Silvergear has no usable card). Getting there found two things:

- **Camera controls are not visible in every scene.** The "gain" register (page 1 0x04) is the auto exposure's gain CEILING, and brightness/contrast (0x18, 0x12) do nothing at all with auto exposure off.
  So they show only when the auto exposure cannot reach its target (luma ~120): a dim scene (maxed out) or a very bright one. In a mid-lit scene it compensates (luma 122 whatever the setting; with exposure
  capped to 151 lines gain 0x20..0xf0 moves luma 26..113). The test now reports "NOT OBSERVED" in mid-lit scenes (luma 25..200) and fails only in dim or saturated ones. This is the sensor's behaviour,
  not a regression, and the earlier research (research/camera-controls.md) measured in dim rooms.
- **An intermittent stall of a video stream, in `examples/uvc-camera` (not in the USB layer).** About 1 camera switch in 8..25 (Rear stream, then Front) left the new stream waiting for ever; the device log
  showed both streams `idle 1`. The example's "host has stopped reading" rule counted "a frame was offered since the last frame that went out" and a live stream whose frame had already gone out was
  declared abandoned during the other camera's one-second turn; being idle it was never served again, so it could never make the progress that ends the idle state. A first rewrite (timing "a frame is stuck")
  failed the same way at a stream restart (the stale frame of the previous session counted). The rule now records the number of offers at each success: idle = offers were made after the last success and none
  succeeded for a second. 89 camera switches and the full tests (Action 2 x 41/41, Silvergear 50/50) show no stall since the final version.

Test fixes made on the way: fps from frame timestamps; white balance (freezes the gains, so no shift in a settled scene); RAM disk recognised; the microphone AGC check resets the volume to 18 dB first (it started at the
36 dB of the previous check, where any room transient is x8 louder: peaks of 12..14k at rms ~90); frame-count floor 56 of 60 (the host loses up to two frames besides the partial first one at a start).
Port selection for two attached cameras: `APX_USB_PORT=5-1` for `tools/test_uvc.py` and `tools/sensor_probe.py`.

Optional diagnostics left in the tree (off by default): build uvc-camera with `-DAPX_DCD_DIAG` and its 5 s status block adds the EP2/EP3 control/status registers (`apx_dcd_diag`, src/tusb/dcd_apx.c) and the UVC transfer
state (`apx_uvc_diag`, src/tusb/uvc.c). Camera mode has no debug link, so the state at the end of the run is read from the log once the image returns to debug mode (the log keeps ~35 lines).
Still unexplained: a stream occasionally loses two frames besides the partial first one at a start (host side: the stale tail of the previous session).
