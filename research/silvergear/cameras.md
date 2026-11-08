# Silvergear 3222882: independent dual-camera support

Both fitted cameras passed RAM-only hardware acceptance on 2026-11-02 using
`dual-camera-test`, build **f088ba5279651fa9**. The owner confirmed the final
physical labels, usable orientation/colors, and normal stock startup afterward.
The screen and both camera modules were connected; the printer was disconnected.
No SPI reads, erase/program, storage writes, audio or printer commands were
part of this acceptance.

## Accepted behavior

- Both sensors identify as **0x39**, page 0/register 1, SCCB wire addresses
  `42/43` (7-bit address `21`). Their commercial part number is not established.
- Each produced a distinct, nonconstant **640×480 NV21** frame, 460800 bytes.
- **Ten consecutive alternating selections** completed with real captures,
  intact raw/preview DMA guards, no camera timeout or hardware error, and the
  expected APXD build response between every switch.
- Each displayed **320×240 NV21 preview at pitch 320 for 45 seconds**. The final
  counter reached 1126 displayed frames across the preview phases. The owner
  confirmed REAR uses the opposite side from the display, and FRONT uses the
  lens above the display facing the operator.
- Power remained on after the owner released the power button. The verified
  SRAM power/DDR path preceded the full SDK upload and APXD handshake.
- BootROM recovery acknowledged; all owned RAM reservations were cleared and
  read back before returning the loader path. The owner then disconnected USB
  and confirmed a normal battery-powered stock startup.

The accepted machine record is camera-test-04/result.json (archived).
Owner observations are recorded separately in
camera-owner-observations.json (archived). Exact loaded
segments, manifest and ELF are archived under `camera-test-04/firmware/`.

| Artifact | SHA-256 |
| --- | --- |
| Verified stock, 4194304 bytes | `4874af74051372363e8c36ff99e369657648eb353950b41fe2a5a00d35acdb9e` |
| Stock application, SPI2200 → RAM02000000, 675328 bytes | `9f9b9e62f9ae552106fbc56d848a17de2ab619f6b1e0fb299ce78fb45f0bc3cd` |
| Accepted SDK ELF | `747259e3fb2489781a781f4dfca66c0470c02fcfec2265f8b7c2af16e5385940` |
| SDK segment at 02180000 | `fc861656c40cb031f4b1e37a59ff3bb0c8c545c5246b500fa7cccbecb3909186` |
| SDK segment at 02220000 | `ab179f648b54066a7fb9ba0b0c20779f914d079aa289affc171b9b217b99bffe` |
| Rear native NV21 | `54f80f547794aa96c83809f4ebcb2e2d2bf892bff4982d7a300213f1ca9f588b` |
| Front native NV21 | `ed11954be41b3c79ad56b54e16686c79975b42e03cb4a6427520ea83ba6a3fbd` |

The DDR stage remains the previously accepted 352-byte image, SHA-256
`02dc8e97b21c688c8b361893a185c6052f3e090ff1759c05a74f736b3953ebca`;
see [power-ddr.md](power-ddr.md) for its calibration/restoration evidence.

## Physical selection and stock evidence

The stock application was analyzed with Ghidra **OpenRISC:LE:32:JT5295** and
cross-checked against the SDK's native binutils disassembly. Resource addresses
below are SPI offsets, not pointers into the mapped application. Application
addresses map to SPI as `address - 02000000 + 2200`.

| SDK identity | Physical lens | Stock index | Selected power-down net | ID39 page0 register31 |
| --- | --- | --- | --- | --- |
| REAR = 0 | Opposite side from display | 1 | PE11 low; PE10 high | `00` |
| FRONT = 1 | Above display, facing operator | 0 | PE10 low; PE11 high | `02` |

PE10/11 are active-high power-down controls, not sensor bus addresses. Both
modules share the SCCB and DVP connections, so only one is enabled. The driver
holds both power-down nets high for 50 ms, selects one low, and waits another
50 ms before clock/bus initialization. The successful two-sensor hardware run
validates that sequence; it does not claim electrical measurement of each pad.

The first implementation equated stock index 0 with SDK REAR. The owner
identified the swapped labels during test 02. The final correction swaps the
power-down mapping and stock mirror flag together, preserving each physical
image's orientation. Tests 01/02 therefore have **old, reversed physical names**;
test 04 and its archived firmware have the accepted assignments. Test 03
stopped when the `/dev/sg0` node was absent, before SDK acceptance.

| Stock address | Evidence and implementation |
| --- | --- |
| `02050c60` | Camera select/probe/init; stock selection global `020a3478` |
| `02050d38` | Probe MCLK 16 MHz before identification |
| `0203108c` | MCLK from nominal 288 MHz; SPR884c[12:8] divisor minus one, SPR8850 bit26, mux8348[11:10]=2, PD10 |
| `02034fa0` → `0202bde4` | SCCB 200 kHz on PD13/15; SPR9090 = 144 MHz/(5×200 kHz)−1 = 143 |
| `0204f624` | Ordered resource-based ID detection; C0 preliminary FC16; ID39 page FD00 |
| `02050ee4` | Matched profile clock and ordered register table, delay after first pair |
| `02050ab4` | Per-sensor mirror/flip writes based on stock camera index |
| `02031638` | Scaled DMA dimensions/ratios and 24-word coefficient copy |
| `020362f4`, call `02036314` | Pass panel configuration +d0, yielding coefficients at SPIa7108 |

The fitted ID39 profile runs at nominal **24 MHz** after identification.
Clock frequencies are derived from stock divider values, not oscilloscope
measurements. Silvergear's SCCB divider and scaler coefficients differ from the
older Action Vision board. Its 24 coefficients are copied to `b18c..b1e8`;
native input is `640×480`, scaled output is `320×240`, and `b060/b064` hold the
scaled Y/VU DMA destinations. Existing frame-completion, one-shot rearm,
cache maintenance and DMA ownership logic are shared.

`research/AnnotateSilvergearCameras.java` verifies the exact application hash
and language before defining/naming the discovered functions and applying
comments. It was executed successfully and the Ghidra program saved. It can be
rerun from Ghidra's Script Manager against the application mapped at 02000000.

## Reproducible resources and APIs

`tools/extract_silvergear.py` requires the verified full-chip hash and generates
`boards/silvergear-3222882/{resources.json,sensors.inc,scaler.inc}` along with
the panel/printer resources. The sensor root is SPI **a9998**, with five
88-byte records starting at a99d8:

| ID | Wire write/read | ID register | Config SPI | Pair-table SPI | Final MCLK |
| --- | --- | --- | --- | --- | --- |
| A0 | 42/43 | 00 | ac398 | b02b8 | 24 MHz |
| 46 | d0/d1 | f8 | ad038 | b05c4 | 24 MHz |
| A6 | dc/dd | fd | adcd8 | b0744 | 24 MHz |
| C0 | 62/63 | 00 | ae978 | b0894 | 16 MHz |
| 39 | 42/43 | 01 | af618 | b0aa4 | 24 MHz |

All five stock configurations describe 640×480. Ordered register pairs are
preserved, with the final ffff sentinel omitted. Their hashes and resource
offsets are in `resources.json`; the manifest hashes the selected board's
sensor table. Only ID39 has been physically tested on this camera. A6 probing
and orientation are additionally covered by the host model; the remaining
profiles are supported from stock evidence without hardware acceptance.

`apx_camera_count()` returns two on Silvergear, one on Action Vision.
`apx_camera_init()` initially selects REAR. `apx_camera_select(id)` rejects
active capture and active printing. The camera service requires capture stopped
and LCD DMA drained before selection; selection preserves the prior preview
enabled/disabled state. It reports selected camera, count, actual native
dimensions and explicit `test_pattern` status in INFO. A fallback pattern never
counts as sensor acceptance.

`apx_camera_files_prepare()` initializes guarded buffers without starting any
sensor or preview. The `dual-camera-test` example waits for host commands after
the expected APXD build handshake. It initializes the LCD lazily when preview
is requested and overlays the selected physical camera name. Printer, audio,
microphone and button initialization are inactive in this diagnostic. No SPI
initialization/program/erase or printer initialization/submission/command
symbols are linked into its accepted ELF.

Native capture can finish with the display off. Preview uses two guarded
115200-byte NV21 buffers; LCD and capture ownership alternate without a CPU
scaling loop. Before reading DVP registers the driver enables its clock gate.
Cold INFO does not read uninitialized DVP registers. These details matter for
starting from a freshly calibrated BootROM rather than a running stock app.

The cumulative `i2c_errors` counter includes expected NACKs while looking for
other stock sensor types. It increased by four per successful identification
on this fitted ID39 pair; final value 56 is **not** 56 failed captures. The
camera error and timeout counters remained zero in the accepted run.

## Repeating the RAM-only check

From the workspace containing both checkouts:

```sh
python3 appotech-sdk/tools/extract_silvergear.py \
  stock-backups/silvergear-3222882-20261102/stock.bin
make -C appotech-sdk/boot silvergear-bringup silvergear-stage
make -C appotech-sdk BOARD=silvergear-3222882 EXAMPLE=dual-camera-test \
  APX_SPI_TRACE=0 APX_LAUNCH_TRACE_MODE=0
PYTHONPATH=apxtool/src python3 apxtool/research/silvergear_camera_test.py \
  --manifest appotech-sdk/build/silvergear-3222882/dual-camera-test.manifest.json \
  --stock stock-backups/silvergear-3222882-20261102/stock.bin \
  --output appotech-sdk/research/silvergear/camera-test-new \
  --timeout 300 --preview-seconds 45
```

Use a new output directory. With power removed, connect both camera modules and
the display, leaving the printer disconnected. Start the command before entering
BootROM by the established method; fully release the entry short, keep power
held for about three seconds after entry, then release and leave USB connected.
The host validates stock/stage identities, verifies the RAM upload, requires
the expected APXD board/build, captures both sensors, performs ten switches,
shows REAR then FRONT, and recovers BootROM with verified RAM clearing. Failed
stage recovery requires a fresh BootROM entry; the script does not blindly
rerun a DDR stage after takeover. `--attach` is for continuing the exact matching
already running SDK build; it is not a cold-launch substitute.

On an already running diagnostic, individual commands remain available:

```sh
apxtool debug camera info
apxtool debug camera stop
apxtool debug camera select rear
apxtool debug camera raw rear.nv21
apxtool debug camera preview on
apxtool debug camera stop
apxtool debug camera select front
apxtool debug camera preview on
```

Pause preview and allow any LCD transfer to finish before selecting a camera;
BUSY is an ownership response. Host downloads refuse existing files.

## Software checks and limits

Passed: 175 apxtool tests; camera low-level/service models for both boards and
the Silvergear peripheral models under ASan/UBSan. Models cover DMA bounds,
completion versus frame-start events, timeouts, NACKs, ten selections, busy
rejection, the physical power-down mapping, ID39 mirror flags, an A6 profile,
cold INFO/prepare, native capture with LCD off, and preview state preservation.
Both board targets built `dual-camera-test`; Silvergear also built
`printer-camera-demo`, including linker bounds and clock transition checks.
The Action Vision diagnostic build is `834d946558e0092a`; the Silvergear
combined demo build is `10048622c68d6d58`.
The combined printer demo was **built, not launched** in this milestone.

JPEG encoding, SD saving, audio coexistence, buttons and printer operation
have not been hardware-verified on Silvergear by these camera checks. Other
stock sensor profiles, manual exposure/white balance, long-duration stress,
electrical timing margins, and printer/camera handoff remain future work.
The first front capture in test 01 was black while producing valid DMA events;
after the owner gave the lenses a well-lit view it produced a real scene. The
acceptance script discards initial frames and retries constant frames so those
do not pass as camera output. Matching the stock application's entire ISP/UI
behavior is outside this camera milestone.
