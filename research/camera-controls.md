# Camera controls and frame rate of the ID 0x39 sensor

Found on the Silvergear 3222882 (both cameras use the same sensor, ID 0x39 at register 1 of page 0, SCCB 0x42/0x43), 2026-11-09, with `examples/sensor-probe` and
`tools/sensor_probe.py` (live register read/write/dump and frame-interval measurement over the APXD debug link). No datasheet was used or found: everything below was
measured, and the register meanings are the ones that behave consistently, not necessarily the vendor's names. The sensor has three register pages, selected by writing
register 0xfd (0, 1 or 2); the SDK leaves page 0 selected.

## Timing

- A sensor line takes **66.6 us** (800 pixel clocks at 12 MHz: the 24 MHz MCLK over two). A frame is at least **500 lines = 33.3 ms (30 fps)**. With auto exposure
  the frame grows with the exposure once that passes 500 lines: 1208 lines = 80.4 ms = 12.44 fps (the stock setting). Measured intervals are steady to 0.05 ms.
- The 80 ms frame the SDK showed from the start was therefore **not a fixed sensor rate**: it is the auto exposure at its longest allowed exposure, which indoor light
  needs. Both cameras sat at 1208 lines (page 0 registers 0x03/0x04 read 0x04b8) in a normally lit room.
- **Page 1 register 0x02 = n limits the longest exposure to n x 151 lines** (stock n = 8 gives 1208). Measured: n = 1..3 give 30 fps (exposure 151/302/453 lines),
  4 gives 24.9 fps (604 lines), 5 gives 19.9, 6 gives 16.6, 7 gives 14.2, 8 gives 12.4. The price in a dim room is brightness (mean luma 12 / 38 / 64 / 86 / 105 / 120 /
  134 for n = 1..7 in the test scene), which is what the UVC "auto-exposure priority" control trades. (Page 1 register 0xcb, stock 0x3c, also bounds the exposure.)
- **Page 0 registers 0x05/0x06 = extra dummy lines per frame** (high/low). Writing 0x40 to 0x06 added 4.25 ms (64 lines), and the increase is additive with the
  exposure-stretched frame. Why it matters: the SoC's one-shot scaled capture has to be re-armed in the blank time after the last active line. At 500 lines that is
  about 20 lines = 1.3 ms. At 320x240 a bare loop re-arms in time (30.03 fps); at 640x480 the cache invalidation of the 460 KB frame in `apx_camera_poll()` makes it late,
  and every late re-arm costs a whole frame: **15 fps instead of 30**. 2 extra lines (0.13 ms) were enough in a bare loop; the UVC layer sets 12 (0.8 ms, 1..3% of the rate).
- Registers **page 0 0x08..0x29** are the sensor's timing (window/clock): writing any of them (0x08, 0x13, ...) stops the frame stream and restoring the value does not
  bring it back. Do not touch them; a re-launch (power cycle of the sensor) recovers.

## Auto-exposure, auto-white-balance and image controls

All page 1 unless noted. "Luma" is the mean of the 8-bit Y plane of the 320x240 or 640x480 frame in the test scene.

| Register | Meaning | Evidence |
| --- | --- | --- |
| 0x32 | switches: **bit 0 = auto exposure**, **bit 4 = auto white balance** (stock 0x15; bit 2 unexplained) | bit 0 cleared: writes to page 0 0x03/0x04 (exposure lines) stick (256 lines = 33.3 ms, luma 24); set: the AE rewrites them every frame. Bit 4 cleared: the colour cast shifts (U 118 -> 99, V 134 -> 162) with luma unchanged |
| 0x02 | longest exposure in units of 151 lines (stock 8) | above |
| 0x04 | **gain** (stock 0xf0; 0x20..0xf0 is the useful range, above 0xf0 nothing changes) | at a fixed exposure luma = 25 / 52 / 77 / 98 / 115 / 129 / 134 for 0x20/0x40/0x60/0x80/0xa0/0xc0/0xf0 |
| 0x12 | **luma gain ("contrast")**, stock 0x88, saturates at 0xa0 | 0x00 / 0x40 / 0x70 / 0x88 / 0xa0 give luma 104 / 105 / 117 / 136 / 150 and sd 51 / 55 / 65 / 77 / 86, chroma unchanged |
| 0x18 | **luma offset ("brightness")**, stock 0x40, HIGHER = DARKER | luma 147 / 141 / 137 / 131 / 125 / 118 / 111 / 94 for 0x00..0x20..0xff, linear, chroma unchanged |
| 0x6e..0x83 | gamma curve (22 entries, 0x00 0x07 0x10 0x17 ... 0xf8) | zeroing any of 0x71..0x7e changes luma and raises chroma |
| 0xa0.. (page 2) | colour correction matrix (two sets: 0xa0..0xa8, 0xac..0xb4) | zeroing them shifts U/V by up to 100 |
| page 2 0x20, 0x3b | opposite colour-cast pair (V moves -110 / +109 when zeroed): white-balance gains | |
| 0x34, 0x35, 0x36 | AE/ISP internals: 0x34 = 0xeb vs <= 0xc0 changes luma by 60; **zeroing 0x36 freezes the stream** | |

Not found (tried by sweeping every register the stock table writes on pages 1 and 2, flipping bit 6 and then writing 0, and the page 0 registers from 0x54 up): a
saturation, hue, sharpness or manual white-balance temperature control. Chroma only moved together with luma or through white balance and the colour matrix. The UVC
device therefore does not offer those.

## What the SDK offers

`include/appotech/camera.h`: `apx_camera_sensor_read/write(page, reg, value)` for experiments, and `struct apx_camera_controls` (per camera: auto exposure, auto white
balance, dynamic frame rate, gain 32..240, contrast 0..160, brightness 0..255 (higher darker), manual exposure lines, frame interval, extra blank lines) with
`apx_camera_controls_set/get/default` and `apx_camera_service()`. Setting only stores (callable from a USB request handler); the service call, in the application's main
loop, writes the changes to the sensor so that it can never interrupt another I2C transaction. The values belong to a camera, and `apx_camera_init()` writes them back after the
stock register table at every `apx_camera_select()`.

Exposure cap with a frame interval and `dynamic_frame_rate = 0`: n = floor(interval / 66.6 us / 151) limited to 1..8. With dynamic frame rate (the default) n stays 8.

## UVC side (`src/tusb/uvc.c`)

TinyUSB's video class answers nothing for the camera terminal and the processing unit, so the SDK puts its own class driver in front of it (`apx_uvc_driver`) that handles
class requests with entity ID 1 (camera terminal) and 3 (processing unit) on a function's control interface and forwards everything else. Each video function has its
own camera: function n controls camera n (0 rear, 1 front).

| UVC control | Entity | Range (default) | Sensor |
| --- | --- | --- | --- |
| Auto-exposure mode | CT | manual (1) / aperture priority = "auto" (8) (8) | page 1 0x32 bit 0 |
| Auto-exposure priority | CT | 0 / 1 (1) | 1 = dynamic frame rate (cap n = 8); 0 = exposure capped to the committed frame interval |
| Exposure time, absolute | CT | 0.7..120 ms in units of 100 us (33.3 ms), manual mode only | page 0 0x03/0x04 (lines = value x 100 us / 66.6 us) |
| Brightness | PU | -100..100 (0) | page 1 0x18, 0x40 neutral; +100 -> 0x00, -100 -> 0xff |
| Contrast | PU | 0..160 (136) | page 1 0x12 |
| Gain | PU | 32..240 (240) | page 1 0x04 |
| White balance temperature, auto | PU | 0 / 1 (1) | page 1 0x32 bit 4 |

The frame descriptors offer any interval from 33.3 ms to 1 s (continuous, step 33.3 ms): hosts round a request to the grid, so 25 fps becomes 30 and 20 fps becomes 15.
The committed interval is the example's frame pacing target (`apx_usb_uvc_frame_interval_us()`), and the exposure cap in constant-frame-rate mode.

## Measured with the real pipeline (`examples/uvc-camera`, Silvergear, rear camera)

| Request | Result |
| --- | --- |
| 320x240, constant frame rate, 30 fps | 28.8-29.1 fps (the 12 blank lines cost about 3%) |
| 640x480, constant frame rate, 30 fps | first 21.5, then 15 fps in a detailed scene: the frames were 85-90 KB and the bulk video path sends about 2.5 MB/s (89 KB = 35 ms, more than the 33 ms frame period). 29.2 fps with the JPEG quality adapted to the send budget; then, with the send path sped up (research/usb-musb.md), **29.3 fps at quality 90** (the budget no longer binds) |
| 15 fps and 10 fps | 14.3-15.4 and 10.4 fps; 640x480 and 320x240 alike |
| dynamic frame rate, any request | at most the request; in this room 12.4-14 fps because the exposure needs 80 ms |
| both cameras open | served in turn, one second each; the camera that has just been selected needs 1-2 s to settle its exposure and white balance |

## A bug this uncovered (fixed)

When the host stops reading a video stream, a stale packet stays in the endpoint. The two video endpoints share their packet RAM buffers (EP2 and EP3), and
`dcd_apx.c` resolved a stalled packet by flushing it after 3 ms so the other stream could write. The flushed packet made the stalled stream's transfer look finished: its
frame carried on, completed frames counted as sent, and the example's idle detection never fired. Two abandoned streams then kept each other alive for ever and the example
switched sensors every second, so the picture of an in-use camera was permanently half-settled and slow (rear 5 fps instead of 12, a colour cast) after any session with the
other camera: this was also why the front camera measured 6 fps all along. Now the flush aborts the other endpoint's transfer (`XFER_RESULT_FAILED`, TinyUSB aborts the frame), and
the example's idle rule counts a stream as abandoned when a frame has been offered to it and nothing has completed for a second.
