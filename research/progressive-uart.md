# Progressive images and UART

The JPEG SD preview service now selects between the independent baseline
hardware decoder and a bounded software decoder based on the bundled
stb_image 2.30. UART0 has an independent polling driver and optional diagnostic
console mirror. Neither feature calls vendor driver routines.

## Software image path

`src/image.c` uses stb's JPEG header parser, Huffman decoder, progressive
refinement and integer IDCT. The SDK processes at most 16 MCUs per poll and
renders one output row per subsequent poll. USB, buttons, watchdog and UART
maintenance continue during coefficient I/O. Eight-bit YCbCr JPEG supports
baseline/progressive 4:2:0, 4:2:2 and 4:4:4; grayscale is also supported.
Quality-100 and custom quantization tables do not require a separate mode.
Hardware progressive decoding has not been established. Encoder downscaling
is unchanged and remains deferred.

The bundled `third_party/stb/stb_image.h` is unchanged. `tools/prepare_stb.py`
generates a derivative in the build directory, adding optional component
allocation, coefficient-access, block-output and maintenance hooks. It checks
its replacement anchors and emits source/generator/output hashes. The manifest
also records the source and generator hashes. This build uses integer output,
no SIMD, TLS, stdio, HDR, linear floating-point conversion, PSD or PIC support.
stb functions are private to the decoder. Large GIF state, PNG/zlib Huffman
state and palette/flip scratch reside in the reserved extra-RAM region rather
than on the 4 KiB stack. The single-job API is deliberately non-reentrant.
GIF LZW prefixes use an iterative, bounded 4096-entry chain in 8 KiB of that
scratch memory, avoiding input-dependent recursion. A solid-color GIF tests
long prefix chains in both the model and on the camera.
`check_image_stack.py` rejects compiler frames above 1280 bytes; the current
maximum is 900 bytes at `-Os` (524 at `-O0`, 672 at `-O2`). Native PNG, 16-bit PNG and GIF tests also verify the
stack guard. The earlier trial's final guard detected its oversized automatic
stb structures, so that build is not accepted despite correct image output.

PNG (including 16-bit converted to 8-bit), BMP, TGA, GIF's first frame and
binary PNM use stb's RGB decode and SDK RGB-to-NV21 conversion. Alpha is
discarded. Inputs are limited to 2048×1536, at most 1 MiB minus 32 bytes in
the SD preview service. Non-JPEG formats must fit their decoded RGB and
intermediate allocations in the supplied workspace; large images can fail
with a range error. Twelve-bit and arithmetic JPEG, RGB/CMYK JPEG in the
paged path, HDR and animated playback are unsupported.

`apx_image_decode_start` receives explicit input, output and aligned workspace
buffers and rejects overlap/overflow. The allocator is a bounded monotonic
arena; the separate 16 KiB picolibc heap is unchanged. The file service reuses
the unused tail of its 1 MiB compressed-input buffer as workspace. NV21 output
uses an even, aspect-preserving size and a 32-byte-aligned pitch. Small images
are centered without upscaling. JPEG source planes are sampled directly into
the bounded display output rather than allocating a full RGB source frame.

Small JPEG coefficient planes fit in RAM. Larger ones use one MCU-band cache
per component and private create-new `0:/APXJ*.TMP` files. Full signed 16-bit
coefficients are retained through every progressive scan. Lossless RLE of
zero coefficients reduces card traffic; it does not change JPEG quality.
Each compressed page has a CRC32 kept in RAM. Writes are read back through
FatFs after visiting a different sector to flush/evict its partial-sector
cache; mismatches allow two bounded retries. Later reads also check the CRC
before unpacking and retry after cache eviction. `backing_retries` reports
integrity retries independently of SD-controller retries. Persistent
corruption fails the job and removes its private files.
Files are preallocated and synced before coefficient writes. FatFs fast-seek
maps are used when they fit, with ordinary seeking as a fallback. Temporary
files are removed on completion, cancel, error and graceful handoff. Abrupt
power loss or removing the card can leave them behind.

Paging temporarily selects a 12 MHz SD clock, then restores the previous
clock. Earlier runs encountered SD errors at 24 MHz and 12 MHz; a 6 MHz
override also failed, so clock reduction alone did not solve the problem.
The SD sector driver now permits at most two reinitialization/retry attempts,
preserving the selected clock and counters and verifying CID and capacity
before replaying a sector. Persistent errors and write protection still fail.
Info reports retries and the most recent failed command/status. The exact
cause of the intermittent controller failures remains unresolved. Completion
acknowledgements now mask unrelated W1C bits instead of replaying every bit
read from the event register. The controller model demonstrates how the old
sequence can lose DATA_DONE when a short RX finishes before CMD_DONE is
acknowledged; `sd-event-ack-model.json` records this regression. Hardware
timing and status readback have not independently established that race as
the cause of every observed failure.
If a controller failure invalidates the volume, cleanup abandons
the old FatFs objects, remounts and deletes only files this job created.
Failure to clean owned handles/files makes graceful storage handoff fail.
The software job deadline is 600 seconds.

One release 2048×1536 trial failed a FatFs transfer at source line 76 after
four scans. Subsequent SD initialization remained not-ready at ACMD41; this
was a persistent failure rather than a recovered CRC retry. USB, battery
sampling and the bottom-of-stack guard remained healthy. The failure,
controller registers and polling observations are retained in
`progressive-uart-release-3m-SD-failure.json`,
`progressive-uart-release-3m-failure-context.json` and
`progressive-uart-release-3m-polling.json`. The stopped job keeps ownership
of any temporary files that cannot yet be removed, so graceful storage
handoff must still refuse them until the card returns. Reseating the card
did not recover this state. A complete camera power cycle restored stock
MSC access; its original 717,914-byte 3 MP JPEG still matched the saved hash.
The first SDK reload did not produce a USB handshake; a further full power
cycle and unchanged-image retry succeeded. The recorded owned-file names
allowed removal of the two remaining files after reloading, without deleting
unrelated files. `progressive-uart-release-SD-reset-relaunch.json` records the
cleanup. This persistent SD failure remains a limitation, independently of
the successful bounded retries seen during other jobs.
The same build failed a second 3 MP trial on the 2 GB card. Issuing the stock
CMD12 stop sequence after that persistent failure and releasing/reasserting
PA2 did not recover it; `progressive-uart-CMD12-recovery-probe.json` and
`progressive-uart-PA2-recovery-probe.json` retain those negative results.
The vendor's CMD12-before-CMD0 ordering has a separately model-tested
candidate in `sd-cmd12-candidate.patch`; it is **not applied** to the accepted
release source or binary. The comparison card uses that unchanged release,
so its results cannot be attributed to this candidate.

The existing APXD operation 8 and `JPEGClient` API remain in use. Info adds
`software`, `source_progressive`, `scans`, backing-I/O counts and
`workspace_used`. `backing_files` is a cumulative count for the job, including
after close; listing the root verifies removal. Software error `status`
records the source failure line. Compatible baseline 4:2:0/4:2:2 inputs still
use hardware. Read software-specific counters and `source_progressive` only
when `software` is true; hardware jobs retain the last software diagnostic
counters while reporting current hardware geometry, state and errors.
The host wait loop polls software jobs every 250 ms, leaving more CPU time
for decoding and paging, while retaining 20 ms polling for hardware jobs.

```sh
make EXAMPLE=jpeg-demo BUILD=build/my-jpeg
apxtool --device /dev/sg0 sdk launch build/my-jpeg/jpeg-demo.manifest.json --reclaim-jpeg-ram
apxtool debug --port 7-1 jpeg preview 0:/jpeg-test-220x176.jpg
apxtool debug --port 7-1 jpeg preview 0:/picture.png
apxtool debug --port 7-1 jpeg info
apxtool debug --port 7-1 jpeg cancel
```

## UART evidence and API

Raw little-endian/no-delay vendor instructions are preserved in
`uart-vendor.disasm`; `AnnotateUART.java` labels the loaded Ghidra program and
`uart-ghidra.txt` records the applied analysis. Main-image offsets map from
file `0x2200` to RAM `0x02000000`. Relevant routines are:

| Address | Observed role |
| --- | --- |
| `02024684` | Receive interrupt handling |
| `020246d4` | Pin routing and PA7 preparation |
| `02024774` | Baud/control initialization |
| `020247e8` | Transmit byte and completion polling |
| `02033a68` | Board initialization at 115200 baud |
| BootROM `0010395c` | Independent RX-pending/acknowledgement evidence |

UART uses SPR `9080` control (`10` enables), `9084` baud divider,
`9088` byte data and `908c` status/acknowledgement. Bit 1 means transmit done,
write `1` acknowledges TX; bit 0 means RX pending, write `10` acknowledges RX.
At 144 MHz, divider is `144000000/baud-1`. SPR `8340[9:5]` selects TX routing,
and `[4:0]` selects RX. The vendor route-0 helper prepares PA7 and sets TX6/RX0;
the normal board route-1 helper selects TX1/RX1. The exposed camera TX pad
works with **TX route 6**. Alternate physical pads remain unidentified.

`apx_uart_init(baud, tx_route, rx_route)` is explicit. RX `-1` preserves the
RX mux field and disables sampling. Independent 512-byte binary TX/RX queues
are serviced by `apx_uart_poll`, also reached through runtime maintenance.
Transmit has a bounded 10 ms per-byte timeout; receive overflow counts dropped
bytes in the software queue. Hardware overrun semantics are not established.
Shutdown stops output, discards queued bytes and restores saved mux/GPIO
fields and control. The divider reads back zero on hardware, so shutdown uses
the board's known inherited divider 1249 when readback is unavailable.

In C, initialization returns zero on success and minus one for invalid
arguments. `apx_uart_write` accepts at most 512 bytes and returns zero when
queued, one when the queue is busy, or minus one for invalid/uninitialized
use. `apx_uart_read` returns the available byte count. Call the poll function
or runtime maintenance regularly; enqueueing does not block until transmission.

APXD operation 14 commands are 0=info, 1=configure (`u32 baud, u32 TX route,
i32 RX route`), 2=write (at most 512 bytes), 3=read (`u32 maximum`, at most
4096), 4=stop, 5=console (`u32 enabled`). `UARTClient` handles bounded queue
submission and drain. The optional nonblocking stdout/stderr mirror preserves
the USB diagnostic ring and counts dropped UART console bytes; it never mixes
diagnostics into command responses.

```sh
apxtool debug --port 7-1 uart configure --baud 115200 --tx-route 6
apxtool debug --port 7-1 uart write message.bin
apxtool debug --port 7-1 uart console on
apxtool debug --port 7-1 uart info
apxtool debug --port 7-1 uart console off
apxtool debug --port 7-1 uart stop
```

Physical TX is checked through `/dev/ttyACM0` at 9600/115200/230400 baud,
including binary bytes and traffic during progressive scans. The live runner opens the CH343 adapter exclusively, waits two seconds after
open and one second after each baud change, and records any byte mismatch.
Some earlier trials were interrupted by empty serial reads; the user later
confirmed that UART had temporarily been disconnected. Those trials are
retained separately from final acceptance. The release suite also encountered
empty 64-byte serial reads after six completed image tests, despite the
camera reporting transmission without a timeout. Reopening the adapter,
without resetting or reconfiguring the camera UART, received a 292-byte
probe and then 60 consecutive 256-byte packets exactly during active SD
paging. The interruption and follow-up are recorded in
`progressive-uart-release-serial-interruption.json` and
`progressive-uart-release-serial-followup.json`. The user subsequently explained
that the unsoldered jumper was being held against the UART pad and slipped
several times during testing. The UART connection is now intentionally
unplugged. The remaining image checks use explicit `--skip-serial`; earlier
exact-byte results remain recorded, and no additional physical UART coverage
is claimed for the disconnected phase. The live suite resumes from its saved
image checkpoint and retains the interrupted reports, rather than presenting
the run as uninterrupted. The already completed 1920×1080 job is validated
with `--adopt-completed`; its elapsed time includes the host interruption and
is not a clean performance measurement.
The user
confirmed that the accessible UART pad is TX-only: **receive is model-tested,
not physically verified**. No RX connection is assumed.

## Validation and footprint

`tools/test_image_model.py` runs the actual decoder and generated header under
ASan/UBSan with file-backed FatFs mocks: all eight original progressive sizes,
4:2:0/4:2:2/4:4:4, quality 100, grayscale, additional formats, forced paging,
cancellation, malformed/truncated input, SD failure/remount cleanup and
deadlines. It also verifies recovery from a corrupted coefficient-page read
and bounded failure for persistent corruption. UART models cover both 96 and 144 MHz, routing, queues, overflow,
timeout, console mirroring and write-only-divider restoration. SD/system/JPEG
models cover the clock API and handoff integration.

The fresh, uninterrupted image suite on the replacement SDHC card passes all
eight progressive originals, fourteen additional image cases, and all eight
baseline hardware cases. The originals are read back against saved hashes.
Quality-100 cases include baseline/progressive 4:2:0, 4:2:2 and 4:4:4; other
cases cover grayscale JPEG, PNG, 16-bit PNG, BMP, TGA, GIF (including a long
LZW chain) and binary PNM. Cancellation and the two negative cases remove
their temporary files. The bottom 512 bytes of the 4 KiB stack remain zero.

| Progressive source | Elapsed seconds |
| --- | ---: |
| 220×176 | 1.89 |
| 320×240 | 2.99 |
| 640×480 | 21.20 |
| 1280×720 | 56.01 |
| 1280×960 | 58.25 |
| 1600×1200 | 85.42 |
| 1920×1080 | 79.84 |
| 2048×1536 | 148.05 |

These are host-observed times with 250 ms software-status polling at the
144 MHz SDK setting, including rendering. This run records 155,739 sector
reads and 54,895 sector writes, **zero SD errors/reinitialization retries**,
and zero coefficient integrity retries. These results apply to this tested
card and do not erase the older SDSC card failures. The complete 2 MiB SPI
readback matches `063e10e7df19095f5ee8217cb031e03297941194f460959a47056c12a38dab66`.
The host suite passes 158 tests plus 6 subtests.

Three same-image BootROM/relaunch cycles also pass. Each handoff starts with
three open progressive coefficient files and a nonempty 9600-baud UART queue.
The host verifies loader INQUIRY, a RAM read and release of every SDK RAM
reservation before relaunching. Temporary files are gone, UART starts disabled,
battery sampling resumes and a new progressive image completes after every
cycle. The last cycle repeats the full unchanged 2 MiB SPI comparison. The
SDK is left showing the 220×176 progressive original with UART configured at
115200 baud, TX route 6; the physical UART adapter remains disconnected.

The 144 MHz acceptance ELF is build `13c3adbe2a78cf17`, SHA-256
`2c542332b46992fd0483afb19f36029ad58f0528d4b46bd773a8fead0e6315dc`.
`progressive-uart-release-elf-audit.json` verifies 1,768 direct calls confined to
SDK executable sections and 6,584 inert branch slots. The intentional
BootROM handoff and SDK callbacks remain the reviewed indirect calls.
`progressive-uart-footprint.json` records sections and reservations:

| Region | Used bytes | Reserved bytes |
| --- | ---: | ---: |
| Runtime, including 16 KiB heap | 120,640 | 131,072 |
| Extra RAM, including code and FatFs tables | 632,320 | 786,432 |
| Display DMA workspace | 63,360 | 65,536 |
| Codec input/output workspace | 1,179,648 | 1,179,648 |
| Separate stack | 4,096 | 4,096 |

ELF size reports text 648,010 bytes, data 57 and BSS 1,351,968. No new RAM
reservation was added. The 96 MHz JPEG build, 144 MHz camera build and non-LTO
software-image object also compile successfully. Camera preview and file
image preview remain separate examples because their buffers share codec RAM.

The reproducible live suites are `tools/test_progressive_uart_live.py` and
`tools/test_progressive_uart_recovery.py`. They use a new output directory/file,
preserve the original card images, create new test files under `SDKJPEG/STB`,
and check the full SPI readback through the independent read-only driver.
`--skip-serial` explicitly omits physical serial checks when the adapter is
disconnected; it does not replace or manufacture UART acceptance results.
Final image results are recorded in
`progressive-uart-replacement-card-live/acceptance.json`, recovery in
`progressive-uart-recovery.json`, and the combined evidence in
`progressive-uart-acceptance.json`. The replacement is a 16 GB SDHC/FAT32 card,
marked Class 10 according to the user; the original 2 GB SDSC card has no
speed-class marking. `progressive-uart-replacement-card-inputs.json` records
all 16 test-image hashes and their independent SDK readback. Copies were
placed only on the camera's physical USB port, preserving the separate card
reader. The host reported a busy unmount before the first replacement-card
launch; all copies had been fsynced, and all 16 were subsequently verified
through the SDK before further card writes.

The interrupted 2 GB composite suite remains failed. Its individually passing
UART binary and console checks, plus the standalone UART follow-up, supply
physical TX evidence. The replacement-card image suite explicitly skips
physical UART because the user unplugged it. Failed trials and their SD
limitations remain in the aggregate; they are not labeled passing runs.
The comparison does not establish whether the older card's behavior or a
controller compatibility issue causes its persistent failure.
The original 2 GB card may still contain private `APXJ*.TMP` files from its
second persistent failure and subsequent physical power-off; that card was
removed before a final cleanup could complete. Cleanup assertions in final
acceptance apply to the replacement card, not to that removed medium.
