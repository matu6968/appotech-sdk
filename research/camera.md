# Independent camera capture

The Action Vision board now has an independent sensor/DVP driver, live LCD preview,
raw frame retrieval, hardware JPEG still encoding and create-new SD photo saving.
The verified sensor configuration is **640×480**, ID **0x39** at I²C address
**0x21**. Its exact commercial part number has not been independently established.
The resource's larger advertised photo sizes are generated from VGA data.

The 96 MHz DMA milestone below used build
`cf6d4f4ae077f799`, ELF SHA-256
`1aea37b9a2a958bd0143c15c3300ebe1e3be7e81c1878a1a41b19608bb8f93a0`.
The current SDK defaults to approximately 144 MHz with the same camera DMA
pipeline. See [clock profiles](clock.md) and `clock-144-acceptance.json` for
its accepted build, timing and coexistence results.

## Use

Commands below assume apxtool's environment is active. From the workspace root,
the direct equivalent is `PYTHONPATH=apxtool/src python3 -m apxtool` followed by
the same arguments.

```sh
make -C appotech-sdk EXAMPLE=camera-demo
apxtool --device /dev/sg0 sdk launch appotech-sdk/build/camera-demo.manifest.json --reclaim-jpeg-ram
apxtool debug --port 5-1 camera info
apxtool debug --port 5-1 camera preview on
apxtool debug --port 5-1 camera capture --output photo.jpg --save 0:/photo.jpg
apxtool debug --port 5-1 camera capture --width 2048 --height 1536 --output photo-3m.jpg
apxtool debug --port 5-1 camera raw frame.nv21
apxtool debug --port 5-1 camera preview on
apxtool debug --port 5-1 camera stop
```

Launch from a fresh stock MSC boot when changing firmware layouts. The existing
warm-loader instruction-cache limitation still applies. `--reclaim-jpeg-ram`
uses the existing bounded, backed-up codec RAM reclamation flow. Capture pauses
preview and keeps its result stable until another capture, preview or stop.
Saving refuses an existing SD filename; host downloads also refuse an existing
file. Formatting is never part of camera startup or photo saving.

The demo starts live preview, retains USB debugging, SD/FatFs, GPIO, watchdog,
speaker and microphone services, and keeps the two-second Power hold policy.
It is a hardware demonstration, with photos triggered through the host interface.
The JPEG codec's baseline decode API remains available in `jpeg.h`; the
`jpeg_files` SD/sample-demo service and `camera_files` service have separate
examples because their static workspaces share the same codec RAM reservation.
Linking both file services together exceeds that reservation. An application
can use the low-level camera and JPEG APIs with its own shared buffer policy.

`include/appotech/camera.h` is the reusable low-level interface: initialize,
start native or hardware-scaled NV21 frames into caller-owned DDR, poll, inspect
status, stop and shutdown. Native destinations need 460800 bytes, aligned to 32
bytes. `apx_camera_scaled_start(buffer,capacity,width,height,pitch)` uses the
camera scaler and one-shot scaled DMA. Its dimensions must be even, within
16..640 × 16..480, with a 32-byte-multiple pitch covering the width, at most 640.
Capacity must cover `pitch*height*3/2`. The stock formula supplies geometry for
that range; physical preview validation covers 220×176 at pitch 224. Only native
capture stops the sensor after every completed frame. Scaled capture retains
the sensor between frames until explicit stop/shutdown or an error. Its complete
cache lines belong exclusively to capture until DONE/ERROR. The application
must not hand that destination to a simultaneous reader/writer. First frames
can precede automatic exposure settling; preview replaces those frames as the
sensor settles. The optional `camera_files.h` service owns static guarded buffers and
must be polled alongside debug, display and buttons.

## Image handling and limits

Receive DMA produces NV21: 640×480 luma followed by interleaved V,U at pitch 640.
Independent host conversion produced a legible image of the camera box; the user
confirmed moving, correctly colored/oriented preview without torn or scrambled
rows. The LCD preview includes the complete sensor field, stretched to 220×176.
It now uses the camera hardware scaler and scaled output DMA, followed by the
independent compositor/LCD DMA driver. Two guarded 59136-byte buffers alternate:
the camera can fill the free buffer while LCD DMA reads the other. Completion
requires status bit 1 and the hardware-cleared output enable; the driver then
invalidates the completed buffer's cache lines. No CPU pixel-resampling loop
runs for preview. If the main loop is held up by SD/USB work, one-shot capture
stops writing after the complete frame and waits for a free buffer.

The user confirmed clean, faster moving preview with correct colors/orientation
and no tearing or scrambled rows. A 60.43-second run with speaker, microphone,
SD reads and USB active delivered **500 frames, 8.27 frames/s**, versus the
previous implementation's 2.77 frames/s under the same coexistence workload.
A separate 60-second USB polling test delivered 745 frames (12.42 frames/s).
Sensor exposure/scene timing can affect frame rate; these figures describe the
measured workloads, rather than a fixed-rate guarantee. Sensor and CPU clock
settings are unchanged. Small still-photo reduction remains
in software; it is separate from preview.

JPEG stills support 220×176, 320×240, 640×480, 1280×720, 1280×960, 1600×1200,
1920×1080 and 2048×1536, baseline 4:2:0. Small photos are reduced in software
inside the completed receive buffer; every destination luma store precedes its
source, and reduced chroma lies below the original chroma plane. Their encoder
source and target dimensions match. Larger photos use the already verified
hardware JPEG upscaler. **Hardware encoder downscaling and progressive decoding
remain deferred.** Sensor exposure, white balance and processing use the selected
stock preset; manual controls and additional ISP features are not implemented.
The JPEG output capacity is bounded at 512 KiB; a complex high-quality image that
exceeds it reports a codec error instead of overwriting adjacent RAM.

## APXD camera service

Operation **11**, MEMORY space, width 1, retains the existing 24-byte APXD v1
header. Its address field selects the command. Payload words are little-endian;
all reads return at most 4096 bytes.

| Command | Request | Result |
| --- | --- | --- |
| 0 INFO | Empty | JSON status and native/photograph dimensions |
| 1 PREVIEW | u32 enabled (0/1) | Pause/resume preview; active captures must finish or be canceled |
| 2 CAPTURE | u32 width, height, JPEG selector (0..15) | Capture a native frame, then encode one of the eight photo sizes |
| 3 READ JPEG | u32 offset, length | Stable completed JPEG bytes |
| 4 SAVE | NUL-terminated UTF-8 SD path, at most 1024 bytes | Create new, write, sync, close; existing files refused |
| 5 STOP | Empty | Cancel capture/encoding and pause preview |
| 6 RAW CAPTURE | Empty | Pause preview and capture a native frame |
| 7 READ RAW | u32 offset, length | Stable completed 460800-byte NV21 frame |

Job states are idle=0, preview=1, capture-for-JPEG=2, encoding=3, photo-ready=4,
error=5, raw-capture=6, raw-ready=7. Camera errors are 0x500 plus INVALID=1,
RANGE=2, BUSY=3, TIMEOUT=4, HARDWARE=5, UNSUPPORTED=6, NACK=7. Codec errors are
0x520 plus the existing JPEG error; file errors are 0x100 plus FRESULT.
INFO additionally reports `preview_dma`, `preview_width/height/pitch/bytes`,
`preview_buffers`, `preview_buffer`, `preview_guard_ok`, and
`capture_scaled/buffer/width/height/pitch`. Top-level dimensions and `buffer`
retain their native raw/photo meaning for existing clients. `preview_buffer`
identifies the most recently submitted complete LCD frame. Pause preview before
reading it through memory debugging; it is only a stable snapshot while preview
is off. Both preview buffers remain inside the existing protected EXTRA RAM
reservation. No operation numbers or APXD framing changed.
The Python `CameraClient` and CLI perform the framing/chunking. Invalid lengths,
unknown commands, overflow, reads during capture, malformed paths and writes to
protected camera RAM are rejected. Raw memory override remains explicit.

## Hardware evidence

`camera-sensor-resource.json` records the original image hash, resource words,
source offsets, setup hash and compile-table comparison. The comparison was made with `tools/extract_camera_evidence.py` (a one-off bring-up script, since removed from the tree; it
checked `src/camera_sensor.inc` against the stock sensor resource at flash offset 0x96704 of `flash.bin.golden`).

Main code mapping is file 0x2200 → RAM 0x02000000. The resource itself is read
from SPI; it is not a main-image RAM mapping. SPI 0x96704 contains one 72-byte
entry with expected ID 0x39, register 1 and slave addresses 0x42/0x43. Config at
0x9678c selects 24 MHz MCLK and 640×480. The 700-byte table at 0x967ec contains
349 ordered register/value pairs and a final ffff sentinel, omitted from
`src/camera_sensor.inc`. Live stock RAM selected precisely this entry
(`camera-stock-ram.json`), and independent bus readback confirmed the ID and
programmed register values (`camera-sensor-readback.json`).

| Stock function | Verified purpose |
| --- | --- |
| 02003144 | PD1 low, PD0 low/high/low reset, 50 ms intervals |
| 02026f7c / 02027080..02027178 | Dedicated I²C initialization, start/stop, byte transfer and NACK |
| 0202af0c | Sensor MCLK pinmux and nominal 288 MHz/12 divider |
| 0202ae88 / 02035ed8 | DVP pins, polarities, byte traversal and sensor dimensions |
| 0202ad58 | Native Y/VU receive buffers and pitch/frame bounds |
| 0202aa8c | Status/IRQ handling; polling SDK keeps PIC IRQ14 masked |
| 02042a68 / 02042f14 | Resource selection and ordered sensor setup |

| SPR | Verified use |
| --- | --- |
| 9090 / 9094 | Nominal 400 kHz divider 71 / controller enable bit 7 |
| 9098 / 909c | Received / transmitted byte |
| 90a0 / 90a4 | Start 80, stop 40, send 10, receive+NACK 28; completion bit 0, NACK bit 7 |
| 8348 | DVP/MCLK selector 15=2 (bits 11:10); I²C selector 18=3 (bits 2:0) |
| 884c / 8850 | MCLK divider bits 12:8 = 0xb / MCLK clock bit 26 |
| b000 | Selected sensor input configuration 286, bit 0 capture enable |
| b004 | Native preset 0; scaled preview bits 17/22 = 00420000 |
| b030 / b034 | Scaler crop, exclusive end in upper halfword: 02800000 / 01e00000 |
| b180 / b184 / b188 | Scaled dimensions and ratios `(input<<16)/(output/2)` |
| b18c..b1e8 | 24 scaler filter words, panel resource SPI 96104..96163 |
| b068 / b060 / b064 | Scaled byte pitch / Y destination / VU destination |
| b008 bit 19 | Scaler parameter latch, clears after a sensor boundary |
| b018 bit 18 | Scaled one-shot DMA, self-clears on completion; bit 21 is stock software rearm policy |
| b01c | Input (height−1,width−1) = 01df027f |
| b050 / b054 | Native receive luma / VU destinations |
| b058 / b05c | Height 480 / luma frame bytes 307200 |
| b014 / b018 | Transfer/event configuration 8006f839 / receive enable bit 19 |
| b00c | W1C: bit 3 frame start; bit 0 native completion; bit 1 scaled completion |

PD10 is the selected MCLK pin. PD2..9/11/12/14 are the DVP group; individual
pixel/sync pad assignments have not been electrically measured. PD13/PD15 are
the stock dedicated control-bus pads. The clock source frequencies are inherited
stock-profile assumptions, with successful sensor transactions and capture as
functional evidence, rather than oscilloscope measurements.

**The critical completion distinction:** the first implementation stopped at
b00c bit 3, repeatedly retrieving the same dark frame. A bounded continuous
capture probe observed 0 → 8 → 9 and produced the correct box image. The final
driver waits for bit 0, then disables receive, clears destinations, and invalidates
CPU cache lines before exposing the frame. Stock error mask 000e0970 aborts the
job. Capture has a one-second deadline, individual I²C waits have a ten-millisecond
deadline, and NACK is explicit. The common maintenance hook polls capture during USB, SD and audio waits, so a
long file operation cannot postpone capture shutdown until the next main-loop
iteration. This does not alter automatic/manual watchdog policy.
Scaled preview follows stock setup `0202b4b8` and wrapper `0202fb34`.
Its completion callback `0202fe88` is registered at event index 1 by
`02030c30`; `0202b474` rearms bit 18 only after the stock consumer chooses its
next destination. The SDK implements that ownership decision in cooperative
polling and keeps PIC14 masked. Repeated bounded probes observed scaled events
0 → 8 → a, output enable clearing, and about 8.33 completed frames/s without
stopping b000. All 24 filter words are compiled explicitly, verified byte-for-byte
by that extractor; nothing depends on inherited scaler state.
See `camera-dma-source-evidence.json`, `camera-dma-scaler.disasm`,
`camera-dma-preview-start.disasm`, `camera-dma-continuous-probe.json` and
`AnnotateCameraDMA.java` (applied to Ghidra's `flash.bin.golden`).

Buffer guards are maintained on complete cache
lines, outside the DMA range. Shutdown stops capture before restoring its own
pin/clock fields; BootROM and power-off invoke it before other DMA shutdown.

`camera-controller.disasm`, `camera-continuous-probe.json` and Ghidra annotations
record the trace. `AnnotateCamera.java` adds verified labels/comments to
`flash.bin.golden`. Generic OpenRISC delay-slot decompilation is unreliable for
this CPU; native raw instructions and live behavior are authoritative.

## Validation and footprint

Current DMA acceptance is recorded in `camera-dma-acceptance.json`; the previous
native-capture milestone remains in `camera-acceptance.json`. Individual reports retain
failures as well as successful follow-up checks. `camera-stills.json` includes an
early host-test failure caused by trying `mkdir` before mounting FatFs; the final
campaign mounts explicitly and verifies every saved file. An early campaign on
the previous build stopped on a dark frame (`camera-live-startup-9b5b`); three
subsequent captures had mean luma about 149 and the complete repeat campaign
passed. The cause of that first dark observation was not isolated between scene
darkness and sensor startup/exposure settling. Keep initial settling frames out
of application still captures.

The previous native-preview milestone passed three camera/speaker/microphone-active
BootROM cycles and a 60.72-second coexistence run with 168 preview frames.
The DMA build passed all eight JPEG sizes and create-new SD readbacks, with RGB
mean errors 2.14–4.03 against captured pixels. Its coexistence run completed 500
preview frames, 56 speaker jobs, 56 microphone jobs and 229376 verified SD bytes.
The DMA-specific test passed 3370 USB status samples, stable snapshots across
64/512/4096-byte boundaries, and ten preview pause/resume cycles. A separate
2 MiB temporary SD write/readback passed while preview continued; its own file
was removed after its hash matched. See `camera-dma-live/acceptance.json`,
`camera-dma-stream/acceptance.json` and `camera-dma-sd-write-coexist.json`.
Three same-image BootROM/relaunch cycles also passed with scaled camera,
speaker and microphone DMA active, including loader INQUIRY, RAM reads and
owned-RAM release verification (`camera-dma-recovery.json`). The final SDK is
left running preview with both guards intact, PIC14 masked and SD mounted
(`camera-dma-final-health.json`). The host suite passed 149 tests and 6 subtests.
The USB-only example builds, and rebuilding camera-demo afterward produces the
identical tested ELF (`camera-dma-build-check.json`).

```sh
python3 appotech-sdk/tools/test_camera_model.py
python3 appotech-sdk/tools/test_camera_dma_live.py --port 5-1 --manifest appotech-sdk/build/camera-demo.manifest.json --output /tmp/camera-dma
python3 appotech-sdk/tools/test_system_model.py
PYTHONPATH=apxtool/src python3 -m pytest -q apxtool/tests
python3 appotech-sdk/tools/test_camera_live.py --port 5-1 --manifest appotech-sdk/build/camera-demo.manifest.json --output /tmp/camera-live
python3 appotech-sdk/tools/test_camera_recovery.py --port 5-1 --manifest appotech-sdk/build/camera-demo.manifest.json --output /tmp/camera-recovery.json
```

Models exercise the actual driver/service under ASan/UBSan: frame-start versus
completion, timeout/NACK, DMA bounds and ownership, clock restoration, software
resampling of spatial gradients, guards, raw/JPEG range failures, cancellation,
create-new files and sync/close errors. Scaled DMA models additionally exercise
frame-start/native-end rejection, waiting for the one-shot enable to clear,
continuous rearm without stopping the sensor, blocked-LCD backpressure, both
buffer guards, display error shutdown, mode switching and deadlines. Actual
APXD receive framing is tested
with camera payloads; this catches the initial missing-operation framing bug.
Native tests cover all eight JPEG sizes with RGB mean errors 2.85–5.03 against
captured pixels, SD readback equality, existing-file protection and transfer
boundaries around 64/512/4096 bytes.

The DMA demo's footprint is recorded in `camera-dma-footprint.json`.
Total linked code/constant size is 599368 bytes, initialized data 36 bytes,
and zero/no-load sections 1335852 bytes. Camera code/tables total 11754 bytes.
Runtime usage including the 16 KiB heap is
121520/131072 bytes, leaving 9552 bytes free. EXTRA RAM uses 761120/786432 bytes,
leaving 25312 bytes free; its two guarded preview buffers total 118400 bytes.
Camera/JPEG workspace remains 985216 bytes in the existing 1152 KiB codec
reservation. The display DMA workspace remains 63360 bytes in its separate
64 KiB reservation. The stack remains 4 KiB. The manifest, preflight, APXD
protection and acknowledged recovery cover the same five reservations.

This work performs no SPI programming or erase. The camera setup table is
compiled into the RAM image; runtime capture calls no vendor sensor, USB,
display or JPEG routines. The ELF call audit verifies all 1185 direct calls stay
in SDK executable sections and all 4625 branch slots are inert. A new full
physical SPI hash was not collected during camera acceptance; the previous
complete readback remains the last physical SPI-content verification.
