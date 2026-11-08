# Silvergear 3222882: power hold and DDR verification

On 2026-11-02 the owner requested power management and DDR checks after the
display-only milestone, specifically to release the power button after an
upload. The earlier unexpected flash changes remain uninvestigated at the
owner's request. These tests performed **no SPI reads, erase or program** and
did not run the full SDK, camera or printer drivers.

## Hardware result

The complete accepted run is `power-ddr-08.json`. The owner released the power
button after loader entry and left USB connected. Power hold passed 292
returning SRAM callbacks over 15 seconds, followed by the DDR stage and memory
checks with the button still released.

- Power settings: PM40 `02 → 06` for the hold, then `16` with stock rail setup;
  PM58 `04 → 0c`; PM5a remained `09`, preserving its other bits. PM44 remained
  `10`, following the stock read-and-echo sequence.
- The hash-pinned stock calibration entry completed, reported ADC calibration
  800, restored low SRAM except the intentional calibration word at `1e14`,
  restored the loader PLL1 pair and re-entered BootROM USB.
- All 4,194,304 bytes at `02000000..023fffff` passed separate full-range writes
  and full-range verification for zero, all ones, `address XOR a5a5a5a5`, and
  `address XOR 5a5a5a5a`. Writing the entire range before verifying it detects
  aliases between blocks and the two memory halves.
- An independent complete host USB read matched the final address pattern.
  Actual and expected SHA-256:
  `43954164e7fe4948c39edd839687d90da1749c6b8a6824199ff84a2ffc761e5a`.
- RAM was cleared and verified. Both diagnostic SRAM reservations were restored
  with readback, stack guards survived, the watchdog policy was restored, and
  final INQUIRY returned `BuildWin Video050Loader 1.00`.
- After disconnecting USB and starting on battery, the owner confirmed:
  **Stock camera starts normally**.

This verifies the transient power hold and the freshly calibrated RAM through
CPU and USB access. It does not establish full-SDK APXD launch acceptance,
cached/DMA stress, camera/printer operation, or temperature/voltage margins.
The PM hold and rail state intentionally remain enabled while running RAM
code; subsequent stock startup/power-off owns those settings.

## Images and reproducibility

| Artifact | SHA-256 / build |
| --- | --- |
| Verified stock backup, 4 MiB | `4874af74051372363e8c36ff99e369657648eb353950b41fe2a5a00d35acdb9e` |
| Known 64 KiB BootROM | `f5b68ff45bff5f4f21c49008ed0a4bcbab01f2a2cb5aeff8f832ee41be460e39` |
| DDR SRAM stage, 352 bytes | `02dc8e97b21c688c8b361893a185c6052f3e090ff1759c05a74f736b3953ebca` |
| Accepted power/DDR probe, 2,076 bytes | `9f0959c721b0d3fdbf941825460f66ff7e918d1b4fef314626c79d21b4ff5d8a`, build `494eb3d4` |

The exact accepted probe is archived as `power-ddr-08.bin`, with its adjacent
manifest. Later source-comment clarification can change the generated build
identity without changing the power/DDR algorithm; the archive identifies the
actual bytes used for this result.

From the workspace containing both checkouts:

```sh
make -C appotech-sdk/boot silvergear-bringup silvergear-stage
PYTHONPATH=apxtool/src python3 -m apxtool.silvergear_bringup \
  --stock stock-backups/silvergear-3222882-20261102/stock.bin \
  --released-file /tmp/silvergear-new-release-confirmation \
  --output appotech-sdk/research/silvergear/power-ddr-new.json
```

Use new output and release-file names. Enter BootROM with USB connected; hold
power during the initial upload. Once `POWER_LATCH_READY` appears, release
power. In another terminal, `touch /tmp/silvergear-new-release-confirmation`
records that physical step. The host then verifies 15 seconds of stability
before enabling rails and invoking DDR calibration. The file is a physical
release confirmation, not a permission or installation mechanism. This
camera disconnected during some extended entry/hold attempts; avoid holding
the button through chat delays. Do not automatically repeat a failed DDR stage.

## Power sequence recovered in Ghidra and binutils

The stock application is mapped from SPI `2200` at `02000000` and analyzed as
`OpenRISC:LE:32:JT5295`. Ghidra decompilation of `0202b2cc` and `0202b54c` was
cross-checked against native binutils in `stock-app.disasm`.

| Stock function | Used behavior |
| --- | --- |
| `0202ace4`, `0202ad6c` | PM even-address read / odd-address write through SPR `88e0/88ec`; busy bit 3, transaction bit 8 |
| `0202b2cc(1)` | Writes PM40 = `06`; the diagnostic preserves other fields and sets hold bit 2 |
| `0202b54c(1)` | PM43 = `e0`; read PM44 then echo it through PM45; set PM58 bit 3, PM40 bit 4, PM5a bit 3 |

PM42 reads `50` in these hardware sessions; its read value does not echo the
stock `e0` write. Exact electrical meanings of every PM status/control bit are
not established. The diagnostic follows the stock sequence and verifies only
the writable hold/rail fields. In particular, it preserves PM44 and unrelated
PM5a bits rather than applying the older board's literal zero/eight values.
The same board-specific handling is now in `src/power.c`; Action Vision keeps
its existing sequence.

## SRAM launch and DDR stage

`boot/silvergear_bringup.c` contains only bounded PM transactions, selected SPR
observations, and bounded DDR word-pattern operations. It contains no SPI
driver, vendor application callback, DMA or peripheral setup. The compiler
and linker reject writable globals or overlap with boot marks/private stack.
The image is fixed at `2000`, request at `3900`, stack guard at `3700`, and
the saved reservation is `2000..3bff`. Every upload and scratch restoration is
read back. ABI/build identity is required before enabling power.

Uninitialized DDR registers are deliberately not read in the power-only
handshake. DDR access requires an explicit stage-completed token, disabled
CPU caches, and the controller's accepted status. The observed post-calibration
status at `f860` is `07`; stock initialization polls `03` or `0f` at earlier
steps. The diagnostic does not require start-control `f800` to echo writes.
After unlocking, observations are restricted to the stock-read DDR registers
`f818`, `f81c`, `f860`, `f8ac`. Pattern requests are aligned, nonempty, bounded
to 4 KiB and restricted to the verified 4 MiB DDR range.

`boot/silvergear_stage.S` temporarily installs verified stock bootstrap bytes
at their native SRAM locations and calls SRAM `10fc` (SPI `12fc`). This is the
stock clock/voltage/DDR calibration sequence, including its UART setup and
PLL/DDR-domain changes, not a promise that every clock register stays at its
pre-calibration value. USB is detached before the clock transition. The stage
clears fresh RAM, restores loader SRAM, restores PLL1 to `1623 / 0b71b000`, and
re-enters ROM USB initialization at `10385c`. The host verifies the marker,
calibration, physical USB port, low-SRAM restoration, and scratch restoration.

The dedicated returning diagnostic disables the BootROM watchdog on each
entry while operating, matching the established SRAM diagnostic policy. It
records the inherited setting and restores it on successful completion. This
avoids making an idle host timeout part of the RAM test. Watchdog restoration
does not release the PM power hold.

`apxtool.silvergear.prepare()` now uses this SPI-free power path; it no longer
loads the SPI rescue payload or probes JEDEC as a side effect of a RAM launch.

## Earlier attempts and verification limits

The `power-ddr-01..07.json` records are preserved. Initial attempts lost USB;
instrumentation isolated a diagnostic callback that sampled uninitialized
DDR registers. Removing those reads allowed the power hold to pass. Another
attempt stopped because an incorrect full-byte PM42 readback comparison
rejected the stock acknowledgement. Runs 05 and 06 completed calibration and
BootROM recovery, but their overly strict DDR-readiness guards rejected the
observed final status before any pattern writes. Run 07 lost USB after setting
the latch while waiting for physical release confirmation. These are not
RAM-test successes. Run 08 is the complete accepted result.

ASan/UBSan models execute the actual power/DDR payload and both boards' SDK
power paths. They check field preservation, PM non-echo behavior, idle/busy
ownership, cold-DDR read refusal, start-control non-echo, DDR bounds, full-range
patterns, corruption and address aliases, watchdog handling and scratch guards.
Python tests execute that C payload behind modeled BootROM RAM commands,
including full independent host readback and cleanup. The separate stage CPU
model checks native control flow/restoration with analog calibration stubbed;
the successful hardware run supplies the calibration/RAM evidence.

Final software checks passed: 173 apxtool tests, the power/DDR payload model
and both board-specific system models under ASan/UBSan, and the stage control-
flow model. Both SDK targets rebuilt successfully: Silvergear
`printer-camera-demo` build `d52d43fbf0e353c1` and Action Vision `usb-debug`
build `f980897ae3dcf013`. These SDK images were built, not launched in this
milestone.
