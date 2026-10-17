# JT5295 / XYX-001-5295C_V03 display evidence

This file records the original PIO milestone. The current driver also supports
independent DMA; see [display-dma.md](display-dma.md) for that implementation and
the correction to the early interpretation of `a85c/a86c` below.

The first independent display driver was `src/display.c`. It reset and
initializes the panel using GPIO and the SoC LCD bus directly, then writes
RGB565 pixels through PIO. It calls no vendor or BootROM display code, enables
no display interrupt, starts no DMA and changes no SPI contents.

## Images, configuration and software trace

`flash.bin` is mapped with `RAM address = file offset + 0x01ffde00` for its
main executable image (file offset `0x2200` maps to `0x02000000`). The panel
configuration is a separate SPI record and is allocated/copied at runtime;
its file offsets must not be treated as main-image pointers.

The record at SPI file offset `0x96000` has type `0x15`, data offset `0x38`
and data length `0x24c`. Its string at `0x96038` is `iLi9225G`. Relative to
that data, byte 32 selects MCU mode (1), byte 33 selects an 8-bit bus, byte
34 selects byte command records, byte 40 gives divider 20, halfwords 52/54
give native dimensions 176/220, byte 88 gives two bytes per pixel, and
bytes 89/90 are both zero. The init-table offset/length are `0x284`/`0x480`
relative to the record's SPI base. This table occupies `0x96284..0x96703`.

Table records are two little-endian words `(operation,value)`: operation 0
is one command byte, 1 one data byte, 3 a millisecond delay, and `0xffffffff`
the terminator. Two command bytes form the panel's 16-bit register index;
two data bytes form its register value, high byte first. The SDK condenses
these into register/value/delay entries. `display-image-evidence.json` records
image/configuration hashes and the extracted byte table.

| Vendor address | Verified role |
| --- | --- |
| `0x0202bcb0`, `0x0202bd14` | Single command/data byte via SPR `0xa858`; wait for `0xa804` bit 31 |
| `0x0202bd68`, `0x0202bdec` | Command/data word, high byte then low byte |
| `0x0202be70` | Clock gate 25 cleared in `0x8840`, LCD control zero, status/config `0x00809f00` |
| `0x0202bf04`, `0x0202bf20`, `0x0202bfb8` | Bus pulse length, signal polarity, width mask |
| `0x0202c05c` | LCD divider SPR `0x887c = (divider-1)<<2` |
| `0x0202c118`, `0x0202c15c` | LCD output packing words and byte count; source addresses are in compositor `c828/c830` |
| `0x0202c1d8`, `0x0202c284`, `0x0202c2bc` | Controller enable/IRQ mask, frame trigger, ISR |
| `0x0202c4dc` | MCU pin mux and GPIO bank setup |
| `0x0202f640` | Byte/word command table interpreter |
| `0x020305c0` | Panel and SoC configuration |
| `0x020030cc` | Actual board reset: GPIO group 1, mask `0x40`, high/low/high with 50 ms delays |
| `0x02002ca4`, `0x02002f78` | Board display open/backlight control: GPIO group 0, mask `0x40` |
| `0x02043660`, `0x020437b4` | SPI configuration loading and board display initialization |

GPIO register masks are **bit masks**, not pin numbers. RS is group 4 mask
`0x04` (SPR `0x8010`, bit 2). MCU bus setup selects mux 16 = function 13,
which is SPR `0x8348[9:6]`. It configures group 2 mask `0x1fe0` and group 4
mask `0x16` for peripheral output, with group 4 mask `0x08` configured as in
the stock MCU bus path. GPIO output, input-direction, peripheral selection
and enable banks used by the vendor helpers start at `0x8000`, `0x8040`,
`0x8100` and `0x8380` respectively, with stride 4 per GPIO group. Reset and
backlight setup also follows the vendor wrapper's pull-disable writes.

`display-inherited-registers.json` contains the initial read-only snapshot.
The inherited engine had control `0x31`, native byte count 77,439, and DMA
packing word `0x02100008` (initially misidentified as a pointer). The SDK disables that engine before resetting the
panel; it zeros the DMA pointers and uses control `0x11` with CPU IRQ bit 4
masked. Initial timing is `a808=6`, `a80c=0x44`, `a810=0x090000ff` and
`887c=0x4c`. Unknown SPRs are not used for exploratory access.

Ghidra labels/comments were added by `AnnotateDisplay.java`, with output in
`display-ghidra-annotations.txt`. The existing vendor program retains its
generic OpenRISC language; annotations explicitly warn that its delay-slot
decompilation can misplace arguments. All register and call conclusions
were cross-checked against native little-endian raw instructions with PC+4
link semantics, using the SDK binutils disassembly in `spi-main.disasm`.

## Panel format and orientation

The manufacturer's [ILI9225G specification](https://www.freecalypso.org/pub/embedded/LCDs/ILI9225G_DS_V0_06_20110228.pdf)
describes the 8-bit MCU interface, RGB565 transfers, entry-mode register
`0x03`, GRAM address registers `0x20/0x21`, GRAM data command `0x22`, and
window registers `0x36..0x39` (sections 7.2.4 and 8.2). Values, analog power
and gamma initialization come from this board's firmware, not a generic
panel library. The stock `0x01=0x011c` and `0x03=0x1030` settings are retained
during initialization. Entry mode is selected again for each queued blit.

Native GRAM is 176 columns by 220 rows. The camera mounts it rotated:
the first photo (`20261013_225455.jpg`, not included) showed native X pointing down
and native Y pointing left, with correct RGB colors. The board's logical
default is therefore 220×176 landscape. Its mapping is
`GRAM(x,y) = (logical_y, 219-logical_x)`. Entry mode `0x1018` makes the panel
traverse the corresponding rows itself, so no software rotation is needed.

| Rotation | Logical size | GRAM mapping | Entry mode |
| --- | --- | --- | --- |
| 0 | 176×220 | `(x,y)` | `0x1030` |
| 90 (board default) | 220×176 | `(y,219-x)` | `0x1018` |
| 180 | 176×220 | `(175-x,219-y)` | `0x1000` |
| 270 | 220×176 | `(175-y,x)` | `0x1028` |

The second photo (`20261013_225836.jpg`, not included), and the user's confirmation,
verify upright text, the expected red/green/blue/yellow/cyan/magenta/white/black
bars, and TL/TR/BL/BR labels at the correct corners, with no visible clipping
or mirroring in the default landscape mode. Fine scan-line patterns in the
camera photograph alone are not evidence of missing pixels.

## Validation and limits

The host command `tools/display_demo.py --test` checks full-frame patterns,
solid-color framebuffer readback, partial fill pixel counts and USB INFO
responses. Final-build results are in `display-final-tests.json`; earlier
results are in `display-pattern-tests.json`. This confirms bus
completion and host responsiveness, not optical readback of each pixel.
`display-final-tests.json` also records repeated redraws with USB status exchanges;
`display-stress.json` records the first landscape build's one-minute run.
`display-elf-audit.json` checks that direct calls remain inside the SDK and
branch slots are inert. `display-host-tests.txt` records existing host tests.

Three final-build launch/display/BootROM cycles passed, including full and
partial redraws, RAM read/write verification, acknowledged recovery, loader
INQUIRY and a RAM read. Results are in `display-recovery-cycles.json`.
After these cycles, a complete 2 MiB SPI readback (`spi-after-display.bin`)
has SHA-256 `063e10e7df19095f5ee8217cb031e03297941194f460959a47056c12a38dab66`,
identical to the original milestone's hardware baseline. The camera was
then relaunched into the display demo. `display-acceptance.json` aggregates
the final validation and exact build sizes.

`tools/test_display_model.py` compiles the actual driver with AddressSanitizer
and UndefinedBehaviorSanitizer against an independent ILI9225G GRAM model.
It verifies all four traversal modes, strided subrectangles, bottom/right
edge pixels, invalid bounds/stride/alignment/address overflow, the per-poll
PIO budget, timeout shutdown and reinitialization. Rotations 0/180/270 have
model coverage; only the board's default landscape mode has upright optical
confirmation on the physical camera.

The driver sends at most 16 pixels per poll and bounds a failed byte transfer
to approximately 5 ms. Calls must be interleaved with USB polling. Panel
GRAM preserves the displayed image while idle. There is no tear synchronization
or DMA acceleration, and bus settling depends on the inherited clock estimate.
Bad source pointers can still fault: the blit API checks arithmetic and
alignment, not whether every source address is readable. DMA/YUV conversion,
display power management and independent cold boot remain future work.
