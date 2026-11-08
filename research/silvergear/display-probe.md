# Silvergear 3222882: first independent LCD output

This milestone is a returning **on-chip SRAM PIO diagnostic**, not the full
SDK runtime. It deliberately avoids the DDR bootstrap, PLL changes, exception
vectors, DMA/compositor initialization, SPI controller, cameras and printer.
The owner requested leaving the earlier flash-change investigation aside and
testing the display first. No flash access is part of this diagnostic.

## Reproduction

From the workspace containing both checkouts:

```sh
python3 appotech-sdk/tools/test_silvergear_lcd_probe.py
make -C appotech-sdk/boot silvergear-lcd-probe
PYTHONPATH=apxtool/src python3 -m apxtool.silvergear_lcd \
  --timeout 180 --hold 15 --output appotech-sdk/research/silvergear/lcd-new-result.json
```

Supply a BootROM session with the display connected while unpowered, keep USB
connected and keep power held until cleanup is reported. The client does not
force loader entry or automatically retry an invoked test. Normal stock USB
entry remains unavailable: the owner reports a black display and shutdown
after roughly one second with USB connected.

The client checks the known BootROM SHA-256, saves SRAM `0x2000..0x3bff`,
uploads and verifies the binary, and requires its ABI/build handshake before
LCD initialization. The fixed-address binary includes read-only tables and
is **not** the position-independent SPI flasher. It uses the existing returning
callback entry assembly, a private stack below `0x37f0`, a 64-byte guard at
`0x3700`, and a 672-byte request at `0x3900..0x3b9f`. The linker requires code
and constants below `0x2f00`, with no writable global data. Maximum compiler
reported callback call-chain stack use is below the guard margin.

Every callback is bounded. The host supplies the panel delays and at most 256
RGB565 pixels per request. The existing watchdog is reloaded only if armed;
its configuration is not disabled/replaced. This diagnostic has its own small
build handshake; it does not provide APXD or claim full SDK launch acceptance.

## Stock evidence

Evidence is from stock SHA-256
`4874af74051372363e8c36ff99e369657648eb353950b41fe2a5a00d35acdb9e`,
main application SPI `0x2200` mapped at `0x02000000`, analyzed with
`OpenRISC:LE:32:JT5295` and cross-checked in SDK binutils disassembly.

| Stock address / SPI offset | Meaning used by this probe |
| --- | --- |
| SPI `0xa7000`, config `0xa7038` | `-----st7789v`, native 240 x 320, divider 15 |
| SPI `0xa7288`, size `0x210` | Command/data/delay table; generated `panel.inc` |
| `0x02002ef8` | Reset high 50 ms, low 100 ms, high 50 ms |
| `0x02034d2c` → `0x0202c654` | Dedicated reset output in `886c/8874`, selector 2; not PB1 |
| `0x02031e70`, `0x02031ed4` | PE2 command/data selection, settling, byte port `a858`, ready `a804[31]` |
| `0x02032030` | LCD clock gate 25, wait 20 ms, controller/status setup |
| `0x020320c4..0x02032230` | Pulse length, polarity, data mask and divider |
| `0x0203269c` | Mux selector 16 = 13; PC mask `1fe0`, PE mask `16`, PE3 input |
| `0x0202b328` | Backlight: PM read `54`, write `55`, preserve all bits except bit 6 |

The probe uses `a808=6`, `a80c=44`, `a810=090000ff`, `887c=56`,
`a800=11`. It sends ST7789 commands `36`, `2a`, `2b`, `2c`, with RGB565 high
byte first. No DMA geometry, compositor coefficient or framebuffer registers
need to be written for this PIO path.

## Register ownership and cleanup

The payload has a compiled allowlist, not an arbitrary SPR write command.
It saves these fields, changes only their masks in shared words, and restores
them in reverse order on END, cancellation or a bounded peripheral failure:

| SPR | Mask / ownership |
| --- | --- |
| `4800` | LCD interrupt bit `10`; USB interrupt mask preserved |
| `8840` | LCD gate `02000000` only |
| `8348` | LCD mux `3c0` only |
| `8388`, `8048`, `8108` | PC LCD pins `1fe0` only |
| `8390`, `8050`, `8110` | PE LCD pins `1e` only |
| `8010` | RS output bit 2 only |
| `886c`, `8874` | Reset masks `38000403`, `100` only |
| `887c` | LCD divider |
| `88e0` | PM transaction/mode mask `140`, restored after each transaction |
| `a800`, `a808`, `a80c`, `a810` | LCD PIO control/timing words |
| `a804` | Configuration `00ff0000`; live W1C status events are cleared, not replayed |
| PM `54` | Backlight bit 6 only; readback verified |

BEGIN refuses an already active LCD or compositor engine before configuring
anything. Cleanup does not replay inherited DMA. PM bus data and LCD GRAM/
status are peripheral state, not restorable general registers. In a completed
session the host verifies saved fields, untouched PLL/SPI/timer/compositor
observations, the private-stack guard, restored SRAM, and loader INQUIRY.
A disconnect prevents that verification and requires a physical power-cycle
before another test; it is recorded as an incomplete test, not success.

## Hardware results, 2026-11-02

`lcd-probe-01.json`: binary SHA-256
`2c98999be723a0acb97ce95f6ef69b34608d9d17c964fe1cddea31be75e04988`,
build `af22cc4e`, 3,512 bytes. Upload and build handshake passed and all 76,800
pixels were acknowledged. The owner saw the color bars and corner labels,
and clarified that the complete image was upside down by 180 degrees.
Thus reset, PIO transfer, RGB565 panel mode and backlight produced visible
output. Individual colors, precise corner clipping and partial updates were
not separately accepted. USB disconnected during the observation hold, so
register/SRAM cleanup was **not** verified for that session.

The correction changes landscape MADCTL `60` to `a0`, and sets the SDK board
default rotation to 3. The SDK reset low time was corrected to the stock
100 ms. Corrected probe SHA-256:
`6fe95ae5744e7fea99c29ece7b189059a649204c169d80dd05ac9b538bb374a3`,
build `f91aa506`, also 3,512 bytes.

`lcd-probe-02.json`: corrected upload and handshake passed, but USB disconnected
after 0.577 seconds during initialization, before the raster. This is not an
orientation result.

`lcd-probe-03.json`: corrected upload and handshake again passed, followed by
a USB transfer failure after 1.103 seconds. The owner subsequently confirmed
the display was in the correct orientation and said power had briefly been
released. This is an optical orientation confirmation, not proof of a complete
new raster or successful cleanup in that session.

`lcd-probe-04.json`: a final short cleanup check was attempted, but failed a
RAM upload readback after 0.336 seconds. The host stopped; neither cleanup nor
SRAM restoration could be verified. No further uploads were attempted.

After disconnecting USB and trying normal battery operation, the owner
confirmed: **stock camera starts normally**. The visible LCD output and upright
orientation are hardware-confirmed; a continuous corrected-frame/cleanup
session remains unverified. No camera or printer was enabled, no DDR/PLL
setup ran, and the test performed no flash reads, erase or program operations.
The unexpected earlier flash changes were not investigated during this work.

## Software checks

The subsequently supplied photo `IMG_6394.jpeg` (owner-supplied photo, not kept in the repository)
confirms upright labels at all four matching corners, the complete border,
and the expected red/green/blue/yellow/cyan/magenta/white/black order. Exposure
does not allow judging grayscale linearity. Its SHA-256 and observations are
recorded in `display-owner-observations.json`. See [power and DDR](power-ddr.md)
for the next separately tested milestone.

The actual C payload runs under ASan/UBSan in
`tools/test_silvergear_lcd_probe.py`. Its model checks all 76,800 pixels, byte
order and address windows, permitted register masks, intervening unrelated
field changes, restoration, active-DMA refusal, malformed lengths, cancellation,
stalled PIO, PM failure and invalid requests. Five Python integration tests
execute the same C payload through modeled BootROM RAM transfers; they cover
the complete raster, handshake, stack guard, cleanup, upload corruption, bad
ROM/build identity and manifest bounds. These are software checks, not optical
or electrical hardware verification.

After the orientation/reset corrections, the Silvergear peripheral models
passed under ASan/UBSan, and the apxtool suite passed 169 tests. Both board
builds were checked separately; the RAM probe is the only new code used on
the camera in this display milestone.
