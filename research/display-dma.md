# Independent JT5295 display DMA

The SDK now feeds the ILI9225G through the stock hardware path: DDR NV21
planes → compositor/scaler (`c800`) → LCD MCU controller (`a800`) → panel.
It calls no vendor display, compositor or cache-maintenance routine. The driver
implements their register sequences independently and polls both engines.
SPI contents are preserved.

## Corrections to the early interpretation

LCD `a85c/a86c` are **output packing configuration, not framebuffer pointers**.
Their stock values are `0x02100008` and zero. The apparent address-bit mask in
`display-dma-address-mask.json` was a formatting-register mask. Moving a
supposed source pointer into a representable range could not fix that error.
Actual source addresses are compositor `c828` (Y) and `c830` (interleaved V,U).
The stock firmware changes them at `0x0202a11c` and from its frame ISR.

Enabling a 1:1 scaler is necessary for the tested path: clearing `c818` bit 7
produced scrambled rows and compositor status `0x200`, despite LCD completion.
Enabling it and setting both ratios to `0x10000` produced compositor completion
bit 0 and the correct geometry. `c818` bit 13 and `c838=0x4000` enable the stock
YUV conversion: leaving the conversion disabled displayed the Y/V/U components
as colors, turning neutral white pink and neutral black cyan. Swapping the
host-generated U,V bytes then established that this board consumes **V,U**.

The first RGB converter used a 220-byte stride, while the working reference
used 224. That produced a diagonally displaced image. The SDK now requires
32-byte pitch and plane alignment, consistent with the vendor's 32-byte row
rounding and allocated buffers. In landscape, pitch is 224; portrait uses 192.
`display-dma-conversion-verified.json` records an exact host/device NV21 match.
Its original build predates only the final CPU compositor IRQ-mask correction.

The original small-strip attempt is superseded. The driver still converts two
RGB rows per poll, but completes the whole source frame before starting DMA.
This gives one frame transaction and keeps source ownership straightforward.
Discarded assumptions and experimental scripts remain in [archive](archive/README.md).
Their completion flags alone are not evidence of correct optical output.

## Vendor trace and implemented sequence

Main-image instructions use `RAM = SPI file offset + 0x01ffde00`; raw native
little-endian disassembly is in `spi-main.disasm`. Image and init-table hashes
are in `display-image-evidence.json`. The original PIO/panel evidence is in
[display.md](display.md). Ghidra labels were corrected and expanded by
`AnnotateDisplayDMA.java`; execution is recorded in
`display-dma-ghidra-annotations.txt`. Generic OpenRISC delay-slot decompilation
remains unsuitable for verifying argument placement on this CPU.

| Vendor function | Verified behavior |
| --- | --- |
| `02029e18` | Compositor clock gate 7, output dimensions, burst/config 15 and IRQ configuration |
| `02029f14` | RGB component widths; this board's 5/6/5 selection contributes `c818=0x50000` |
| `02029fb4` | Conversion parameters `c838=0x4000`, enable `c818` bit 13 |
| `0202a070` | Main-plane traversal selector; zero is used for the tested linear planes |
| `0202a11c` | Y and V/U addresses in `c828/c830` |
| `0202a138` | Byte pitch divided by four in `c82c/c834` |
| `0202a844` | Scaler coefficients, dimensions, 16.16 ratios and enable bit 7 |
| `02029d50` | Frame trigger: compositor control bit 1 |
| `02029d98` | Compositor completion bit 0 in `c80c` |
| `0202c118` | LCD packing words `a85c/a86c`, selectors `a868` |
| `0202c15c` | LCD byte count minus one in `a854` |
| `0202c284` | LCD frame trigger bit 1 |
| `0202c2bc` | LCD completion bit 3, acknowledged through status bit 11 |
| `02022c98` | Range maintenance: up to 4 KiB, 16-byte line count in `181c`, address in `180c` |

Initialization stops both engines, masks CPU LCD IRQ 4 and compositor IRQ 21,
clears clock gates 25 and 7 in `8840`, and independently initializes the panel.
Compositor `c808` is zero. Other planes, gamma and auxiliary transforms are
disabled. The 40 scaler coefficient words from vendor `0205dc4c` are written
explicitly; none are inherited. Detailed meanings of every coefficient and
the stock `c81c=0x707` parameters remain unverified; the exact values are tested.

For each frame, the driver programs the panel window and GRAM cursor using PIO,
cleans both source planes, and writes:

| SPR | Value |
| --- | --- |
| `c810/c820/c928/c92c` | `(height-1)<<16 \| (width-1)` |
| `c828/c830` | CPU DDR addresses of Y and V,U planes |
| `c82c/c834` | Aligned byte pitch / 4 |
| `c920/c924` | `0x10000` (1:1 ratios) |
| `c818` | `0x52081`: RGB565 output, conversion, scaler and main plane |
| `a840/a844` | Frame width/height |
| `a848/a84c` | Width/height minus one |
| `a854` | `width*height*2-1` |
| `a85c/a86c/a868` | `0x02100008 / 0 / 0` |

It enables LCD control `0x11`, starts the compositor with `c800=3`, then triggers
LCD control `0x13`. Completion requires `a804` bits 31 and 3 **and** `c80c` bit 0.
The driver then disables the compositor, clears its plane enable and addresses,
acknowledges both events and releases the source. A one-second deadline stops
both engines and switches off the backlight on failure. Init and BootROM return
also stop both engines before clearing addresses. USB polling remains cooperative
through commands, conversion, transfers and recovery.

## Public interfaces and memory

`apx_display_blit_dma` preserves a contiguous, even-sized RGB565 DDR source and
converts it to full-range BT.601 NV21 using small integer contribution tables.
The converter averages chroma in 2×2 blocks and may soften isolated colored
pixels. Existing PIO blit/fill APIs preserve exact RGB565 and support odd or
strided rectangles. Rotation is still handled by panel traversal.

`apx_display_blit_nv21` accepts two 32-byte aligned DDR planes with a shared
32-byte aligned byte pitch and even dimensions. It performs no CPU conversion.
Source pointers remain owned until READY or ERROR. RGB sources overlapping the
private conversion workspace are rejected. Arithmetic, alignment and DDR bounds
are checked before queuing a request.

The linker reserves `02210000..0221ffff` only when the display workspace is
linked. It contains 63,360 bytes, enough for the largest supported portrait frame.
Landscape consumes 59,136 bytes. The original 128 KiB runtime, 4 KiB stack and
16 KiB heap remain unchanged. Manifest generation, launch preflight, APXD memory
protection, INFO ownership and acknowledged BootROM release all include the
optional 64 KiB reservation. USB-only builds still advertise two reservations.

## Validation and practical limits

The camera user confirmed clean geometry/colors for native full frames and the
final converted frame; `IMG_20261014_114705.jpg` records the latter. Earlier
photos and failed probe reports are retained to distinguish faults from success.
Other rotations have model coverage; physical optical acceptance covers landscape.

The sanitizer model runs the actual driver against an ILI9225G GRAM model and
an independent NV21 color decoder. It covers full portrait/landscape frames,
all four rotations, 32-byte pitch, source preservation, bounds, workspace overlap,
IRQ masking, completion and timeout shutdown/reinitialization. Host tests cover
optional reservation validation, occupied-workspace refusal, and release only
after acknowledged recovery plus loader INQUIRY and RAM read.

`display-dma-acceptance.json` records the final build, linked code/RAM sizes,
model/host tests, exact-call audit, sustained redraws, ten recovery cycles,
source readback and SPI comparison. Benchmarks distinguish driver ticks from
example rendering and host elapsed time. Native frames take roughly 17 ms;
RGB conversion plus DMA roughly 66 ms; PIO roughly 255 ms. This is about 58,
15 and 4 driver frames/second respectively, before application rendering.
There is no tear synchronization, cold-boot initialization or general DMA API.
