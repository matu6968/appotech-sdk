# JT5295 independent JPEG engine

This report records the original hardware baseline bring-up. The current SD
preview service also supports progressive/4:4:4 JPEG and additional image
formats through a bounded stb_image software backend; see
[progressive images and UART](progressive-uart.md). Hardware progressive
decoding and encoder downscaling remain unverified.

The SDK implements polling baseline JPEG decode and encode through SPR
`c000..c148`, with programmable tables in on-chip SRAM `66000..66c40`.
Decode produces full-range NV21 for the existing compositor/LCD driver.
Encode consumes NV21 and generates the complete baseline 4:2:0 JFIF stream.
No vendor or BootROM codec routines execute after SDK takeover.

## Evidence and confidence

The main SPI image maps file offset `2200` to RAM `02000000`; raw
little-endian/no-delay instructions in `spi-main.disasm` are authoritative.
The existing Ghidra main program is `flash.bin.golden`. Its generic OpenRISC
decompilation assumes delay slots and can invert branches or invent function
boundaries. `AnnotateJPEG.java` adds labels/comments at raw traced entry
sequences and removes auto-named functions created by the initial trace.
`jpeg-ghidra-annotations.txt` records the applied annotations. `TraceJPEG.java`
now inventories existing functions without creating new boundaries.

| Stock address | Observed behavior | Confidence |
| --- | --- | --- |
| `0202cc10` | Active-low clock gate 31 in `8840`, reset bit 2 in `8880` | Raw sequence and live independent operation |
| `0202ccd0..0202d628` | Separate `b800` camera/stream encoder setup and wait | Raw only; not the SDK encode path |
| `0202d694` | Horizontal prescaler, reduction above 1920 pixels, extra power-of-two reduction, 16.16 ratio | Raw sequence and eight live decode sizes |
| `0202d764..0202d9a4` | Source/output geometry, Y/VU and entropy addresses | Raw sequence and frame readback |
| `0202da0c` | Copy 796-byte decoder table block to `66400` | Raw plus parsed custom JPEG tables |
| `0202da50/0202dcf4` | Decoder initialize/start, `c000=60000023` for 420, bit 16 for 422 | Both sampling modes verified live |
| `0202ddc8/0202df60/0202e608` | Stop/reset, busy and completion polling | Raw plus completion/timeout model |
| `0202e058..0202e3bc` | Shared `c000` core encode, source planes/output bounds/start | Complete JPEGs verified at eight sizes |
| `02037318/020377f0` | SOF0 parser accepts Y sampling `21`/`22`, Cb/Cr `11`; configures decoder | Raw; no verified progressive path |

The exact register bit names remain inferred. Confidence applies to these
tested sequences, rather than a complete SoC register specification.

## Decode

`apx_jpeg_inspect` validates marker bounds, eight-bit SOF0, three ordered
components, quantization tables 0/1, canonical Huffman tables 0/1, a single
interleaved SOS, stuffing/restart sequence and an EOI. It rejects progressive,
4:4:4, grayscale, multiple scans, unsupported tables, malformed lengths,
missing tables, zero quantizers, oversubscribed/duplicate Huffman symbols,
unsupported symbol sizes and missing EOI before starting hardware. It does
not decode entropy on the CPU or prove that every entropy-coded MCU is valid.

The decoder table at `66400` contains two 64-byte zigzag quantizers, four
16-element little-endian canonical base-code arrays at offset 256, cumulative
symbol counts at 384, and symbol arrays at offsets 448/460/472/634. The stock
filter coefficients and `c148=22105105` are loaded directly by the SDK.

`c00c` is source width/height; `c010/c014` are output Y/VU addresses.
`c020` is the word-aligned entropy address and `c02c` its starting bit offset.
`c034` receives the restart interval. `c04c` selects component quantization and
Huffman tables. `c078` contains the requested output geometry;
`c07c/c080` contain horizontal/vertical scale settings. Critically, `c0c0`
holds source width in its high half and output byte pitch in its low byte.
Leaving this pitch zero repeats rows; reversing the 420/422 selector causes
MCU/color corruption. These were corrected and verified against frame data.

Output pitch is `round_up(width,32)`; sources up to 2048×1536 currently scale
to at most 224×220. The public API uses aligned DDR buffers with non-overlapping
input/output regions. Input includes 32 zero guard bytes. The stock entropy
sequence sets `c024=0`; an independently verified decoder DMA input-end
register has not been established. Structural validation and a two-second
hardware timeout are provided; arbitrary corrupted entropy is not a trusted
DMA sandbox. No full-resolution decode API is claimed in this milestone.

Preview reads bounded FatFs chunks, closes the input, preserves aspect ratio,
centers the result and uses one NV21 compositor/LCD transfer. The framebuffer
remains owned until display completion. 4:2:2 input also produces NV21; the
native 422 test has RGB mean absolute error 2.062/255 against host decoding.
`jpeg-422-live.json` additionally verifies 422 sources at all eight resolutions.

## Encode

The shared core receives target geometry at `c00c`, source geometry at
`c078`, 16.16 scaling at `c07c/c080`, and source Y/VU pointers at `c020/c024`.
`c010/c014` bound the output stream; `c014` includes the stock `30000000`
control bits. `c000=0040ec01 | (selector<<23)`, `c03c=80000055`,
`c030=03000258`, trigger bit 8 and the `c0a4=8; c0a4=800` sequence start it.
Status bit `40` signals completion. `c01c` reports an **eight-byte-aligned**
end pointer: the SDK removes at most seven padding bytes and requires SOI/EOI
before reporting success. Native output length is 7597 bytes, while hardware
reports an aligned 7600-byte end; 640×480 produces 36547/36552 respectively.

The JFIF/header table block begins at `66000`. Quality-90 quantizers are
written in zigzag order. Canonical Huffman counts/symbols use the baseline
Annex K tables in `src/jpeg_tables.inc`. AC symbol-indexed packed codes at
`66800/66a00` and DC codes at `66c00/66c20` use
`((length-1)<<12) | (code & 0xfff)`. The 12-bit mask is essential: otherwise
long AC codes overwrite the length bits and corrupt encoded blocks. Packed
tables matched the stock SRAM tables during probes, while the SDK rebuilds
them independently at each job.

The initial API accepts even NV21 geometry, 32-byte-aligned planes and pitch
`round_up(source_width,32)`, bounded aligned output, and target dimensions at
least as large as source. Encoder downscaling is rejected: the traced stock
sequence clamps its source window and the initial downscale probe corrupted
blocks. A native 220×176 sample avoids this unsupported path and hardware
scales it to every requested resolution. The selector's quality mapping is
not calibrated; acceptance uses selector 8. The SDK is a single-job engine,
not an ISP capture or continuous MJPEG pipeline.

## Runtime, ownership and host operations

Core API: `appotech/jpeg.h`. Optional SD/display jobs: `appotech/jpeg_files.h`.
`jpeg-demo` runs USB/debug, GPIO/buttons, watchdog, display and JPEG jobs
cooperatively; Power hold still requests graceful shutdown.

The optional service reserves `022e0000..023fffff` (1152 KiB): a 1 MiB stream
and 128 KiB NV21 workspace. Codec code/tables occupy 13,143 bytes in the
existing `02220000..022dffff` extension, beside FatFs's 483,036-byte Unicode
tables. Runtime use is 121,552/131,072 bytes, including the separate 16 KiB
heap; stack is a separate 4 KiB. Display DMA workspace uses 63,360 bytes.
The manifest records reservations, sections, hashes and toolchain provenance;
`jpeg-footprint.json` records this build's measured footprint.

The loader normally leaves vendor data in the new codec workspace. Explicit
`sdk launch --reclaim-jpeg-ram` backs up precisely that occupied arena to a
new `.bin` and `.json` beside the manifest, clears/verifies it, and restores
the original bytes if pre-takeover work fails. Other reservations retain
strict zero preflight. After takeover the host never performs callback
cleanup against the active SDK. BOOTROM acknowledgment/loader INQUIRY/RAM
verification precede releasing owned RAM.

APXD v1 retains its 24-byte header and 4096-byte maximum payload. Operation 8
addresses commands 0=JSON info, 1=preview (NUL-terminated UTF-8 path),
2=sample encode (`u32 width,height,selector` followed by path), 3=cancel.
Commands start a job; the host polls info until done/error. Error values
`200+codec_error` distinguish invalid/unsupported/range/busy/hardware/timeout;
`100+FResult` preserves storage failures. Existing encode files are refused.
Save success requires complete writes, `f_sync` and `f_close`.

Shutdown stops both JPEG control paths, clears DMA pointers, resets the shared
block and gates its clock. The existing BootROM/power handoffs call this hook.
Changing firmware layouts in the warm loader remains a documented platform
limitation; repeated software reboots after probing sometimes failed USB
enumeration and required a physical reset. A constants-only update with the
same layout loaded successfully through verified BootROM recovery.

## Acceptance artifacts

`jpeg-final-live-v2/acceptance.json` binds the live tests to build
`7b801bc3d5be10f8`. It contains all eight baseline 420 decodes, all eight
hardware encodes, all eight encoded-file decode/display round trips, a 422
decode, cancellation and negative cases. Complete host JPEG decode verifies
encoded geometry/sampling and image content; SD bytes also match the engine's
RAM stream. PNG/NV21 captures and JPEG files accompany the report. Timings are
engine ticks at the inferred 32.768 kHz rate and exclude file I/O/display.

| JPEG size | Decode to screen (ms) | Encode native sample (ms) | Encoded bytes |
| --- | ---: | ---: | ---: |
| 220×176 | 2.72 | 2.38 | 7,597 |
| 320×240 | 4.88 | 3.23 | 16,047 |
| 640×480 | 14.50 | 8.15 | 36,547 |
| 1280×720 | 42.79 | 21.48 | 80,486 |
| 1280×960 | 35.83 | 28.17 | 105,708 |
| 1600×1200 | 57.10 | 43.18 | 156,458 |
| 1920×1080 | 56.64 | 46.84 | 157,902 |
| 2048×1536 | 102.84 | 69.82 | 230,930 |

The sustained run completed 52 decode/display jobs in 61.48 seconds, with
212,992 bytes of explicit SD readback in addition to file downloads. USB
handshakes, GPIO reads and watchdog status remained responsive. SD errors and
ADC timeouts stayed zero; the bottom 512 bytes of the 4 KiB stack stayed zero.
The user confirmed the integrated native image has correct colors and no
scrambling. Original progressive 4:4:4 files were hashed unchanged; separate
baseline copies were used. This initial hardware-only build rejected
progressive files. The later [software backend](progressive-uart.md) adds
progressive decoding without claiming a hardware progressive path.

`jpeg-elf-audit.json` passes 936 direct calls confined to SDK executable
sections and 4049 inert branch slots. Indirect calls were reviewed as SDK
callbacks and the intentional BootROM handoff. JPEG model tests cover bounds,
DMA ownership, packed Huffman codes, completion/error/timeout, 0–7 output
padding bytes and rejection beyond that bound. Existing USB host tests pass
(142 tests, six subtests), and display/SD/FatFs/GPIO/system models pass.
`jpeg-recovery-v3.json` records same-image codec→BootROM→launch cycles;
`jpeg-spi-preservation.json` and `spi-after-jpeg.bin` record the full 2 MiB
read-only SPI comparison, unchanged at SHA-256
`063e10e7df19095f5ee8217cb031e03297941194f460959a47056c12a38dab66`.
SPI callbacks require a fresh stock entry, outside same-image recovery tests.
The initial combined test (`jpeg-recovery-v2.json`) passed two cycles before
the layout-changing SPI callback reset the device. The subsequent stock entry
completed SPI readback and restored its callback RAM; physical reset preceded
the final SDK launch. Failed intermediate probes remain research history,
including `jpeg-final-live` and `jpeg-recovery.json`; they are not acceptance
results. Temporary register-level probes are archived as text.
