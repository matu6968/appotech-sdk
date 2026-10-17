# JT5295 SDK clock profiles

The SDK now selects the accepted approximately **144 MHz** PLL1 profile at
startup, before initializing its independent peripherals. `CPU_MHZ=96` retains
the loader baseline. Absolute MHz remains inferred from the reference and PLL
coefficient; the startup loop independently measures the relative speedup.

```sh
make -C appotech-sdk EXAMPLE=camera-demo CPU_MHZ=144
make -C appotech-sdk EXAMPLE=camera-demo CPU_MHZ=96
```

Changing the option updates the build ID, flags and manifest without a clean
build. The default is 144. Manifest policy is `sdk-pll1-144` or
`inherit-loader-96`; apxtool accepts only those two fixed policies. Both require
a known 96 MHz loader entry. The host does not start a separate clock callback
or restore a clock companion over an active SDK. A changed RAM image still
requires a fresh physical boot because of the existing warm-loader limitation.

## Transition and recovery

`src/clock_pair.S` reuses the accepted loader's coefficient/control ordering:

| Profile | SPR 885c coefficient | SPR 8858 control |
| --- | --- | --- |
| Loader 96 | 0b71b000 | 1623 |
| SDK 144 | 112a8800 | 1123 |

Each critical sequence begins on a 32-byte boundary and holds both writes and
the complete 500-count settling loop in 28 bytes. Its instruction bytes match
both accepted loader blocks from `apxtool/firmware/clock_pair.inc` (since removed with the clock experiments; the accepted transition lives in `src/clock_pair.*`) and the
archived `clock-aligned-hold-payload.bin` (SHA-256
`9b02950c8a7a18edf8971d3a5cffaaaffc45425fe09cfbc54e8b5567af2d2728`).
The delay is a vendor-derived settling count, not a verified PLL-lock poll.
The SDK linker fills executable alignment gaps with native inert NOPs; this
also avoids interpreting zero alignment fill as a jump during instruction audit.

`check_clock_elf.py`, run by every manifest build, verifies alignment, all
28 critical bytes and the fixed constants. `audit_elf.py` separately verifies
SDK call destinations and inert branch slots. Transition code is stackless,
uses immediate constants, calls no vendor/ROM function and writes only the two
PLL1 registers. It does not modify routing, PLL2, peripheral dividers, memory
timing, timer configuration, cache controls or interrupt state.

`apx_clock_init()` accepts only the loader PLL pair with timer control 721 and
reload 3fff. It records a fixed million-iteration benchmark before and after the
transition and checks PLL readback. An unsupported incoming state or failed
readback resets through the watchdog before peripheral startup. The 96 MHz
build issues no PLL writes. `appotech/clock.h` exposes initialize, status and
restore; live arbitrary frequency changes are not exposed.

BootROM return, watchdog reboot and power-off quiesce camera, microphone,
speaker, JPEG, buttons, LCD and SD before restoring the loader pair. USB is
already detached when the downward transition occurs. A mismatched outgoing
pair is not blindly overwritten; recovery takes the watchdog reset path.
The returning C ABI probe also restores the loader pair before returning.

APXD INFO retains its framing and adds `clock`: requested MHz, raw PLL1 pair,
baseline/active loop ticks and error. `cpu_hz_estimate` reflects the active
profile rather than a hard-coded 96 MHz value.

## Peripheral timing

The existing camera, SD, LCD, ADC and reference-timer register settings are
retained. They follow the stock profile; physical pin frequencies have not
been scoped. USB remains high speed with the existing independent driver.

Speaker selector and microphone rate constants depend on `CPU_MHZ`, while the
shared audio divider remains b:

| Requested rate | Speaker selector at 96 / 144 | Microphone control at 96 / 144 |
| --- | --- | --- |
| 8000 | 8 / 10 | 001f4000 / 005f4000 |
| 16000 | 4 / 6 | 000f4000 / 00174000 |
| 32000 | 0 / 2 | 00074000 / 000b4000 |

The initial 144 MHz 8 kHz microphone guess, 002f4000, produced approximately
24 kHz; the rate field did not support the assumed larger count. A bounded
probe using the stock path's extra bit 22 established 005f4000 at 8 kHz.
Its full analog/filter field interpretation remains undocumented. The failed
rate assertion and intermediate probe are retained in `clock-144-rates.json`,
`clock-144-mic-divider-probe.json` and `clock-144-mic-divider-5f-probe.json`.
The final constants are tested at all three rates against reference ticks.

## Validation

The build-bound acceptance and final state are recorded in
`clock-144-acceptance.json`. Raw reports include startup loop timing,
reference-timer cadence, three speaker/microphone timing trials per rate,
eight JPEG sizes and SD readbacks, sustained DMA preview with audio/SD/USB,
ten preview pause/resume checks, a temporary SD write/readback and three
DMA-active BootROM/relaunch cycles. Existing-file protection is retained;
new photo paths are recorded and only the dedicated temporary transfer file
is removed. No SPI programming or erase is part of this change.

The models test actual clock policy code under ASan/UBSan, incoming PLL/timer
guards, readback failure, idempotent initialization, unknown outgoing states and
rollback. Audio/microphone models run for both clock configurations. Existing
camera, watchdog/power and host suites also pass. CPU speedup and preview rates
are reported separately because sensor exposure, scene and DMA cadence affect
preview performance.

The final camera build is `692cf592630e7b52`, ELF SHA-256
`d3826f8839fb7df721a286ba12f7ed8eaf67d2c5aa0b233290ae249f58dee35a`.
Its startup benchmark measured 2390 baseline versus 1593 active ticks
(1.5003×); the first revision measured 2389 versus 1593 (1.4997×).
The reference timer remained approximately 32768 Hz against host wall time.
All nine combined speaker/microphone timing trials passed: microphone
7999.76–8000.00 / 16000.00 / 31999.02–32000.00 samples/s and speaker
8003.14–8003.38 / 16006.07 / 32011.93–32013.88 samples/s.
These use reference ticks, rather than an external acoustic clock measurement.

The 60.52-second camera coexistence test delivered 750 preview frames
(**12.39 frames/s**), 57 speaker jobs, 57 microphone jobs and 233472 verified
SD bytes, versus 8.27 frames/s in the previous 96 MHz campaign. All eight
JPEG sizes passed with RGB mean errors 1.85–3.57 and matching create-new SD
readbacks. The user confirmed clean moving preview after the PLL change.
The initial three-second preview count is retained but is not used as a
sustained rate claim; sensor settling and scene/exposure can change cadence.

The image links 600644 code/constant bytes, 36 initialized data bytes and
1335884 bytes in zero/no-load sections. Runtime including the 16 KiB heap
uses 122832/131072 bytes (8240 free); EXTRA RAM remains 761120/786432 bytes
(25312 free). Stack and all five RAM reservations remain unchanged.

The USB polling check sustained 373 frames in 30.01 seconds (12.43 frames/s),
with 1748 status exchanges and ten pause/resume cycles. A separate 2 MiB SD
write/readback passed while preview ran, and its temporary file was removed.
Three camera/speaker/microphone-DMA-active BootROM cycles passed with fresh
144 MHz startup benchmarks and verified loader/RAM release checks each time.
The final image is left running preview in high-speed USB mode with SD mounted.

The host suite passed 153 tests and six subtests, including rejection of a
144 MHz handshake mismatch without restoring callback RAM over the active SDK.
Both CPU configurations, the USB-only example and returning ABI probe build;
rebuilding the final camera image produces the identical tested ELF. The
96 MHz configuration was built and modeled in this change; new live acceptance
uses the requested 144 MHz profile. Power-off/reboot integration is covered by
the actual shutdown model; physical power-off was not repeated for this clock
change. No new complete physical SPI readback was collected.
