# Thermal-head initialization comparison — November 7, 2026

**Visible printing now works with the normal SDK driver at 100% energy.**
The owner recognized the three-bar sparse pattern both with the isolated stock
reference on battery, then with production firmware `bf7db22520953000`
while USB remained connected. The missing dependency was SPR `8868[12:0] = 182e`, the analog setup
already used by camera initialization. Forced BootROM launches inherited `0190`.

The production driver saves this field, applies `182e` with the camera OFF and
actuators low, then restores it after turning heater, motor and PB6 off. Bits
outside `1fff`, including the battery-reference bit15, remain untouched. The
public printer API and normal APXD commands are unchanged. The physical rail
or bias controlled by these fields remains unidentified; its functional effect
on printing is demonstrated, while an electrical explanation remains an inference.

## Reproducible stock evidence

The application SHA256 is
`9f9b9e62f9ae552106fbc56d848a17de2ab619f6b1e0fb299ce78fb45f0bc3cd`;
the full backup SHA256 remains
`4874af74051372363e8c36ff99e369657648eb353950b41fe2a5a00d35acdb9e`.
Native JT5295 instructions, not generic SPR decompiler output, establish the
operations below. From the SDK directory:

```sh
python3 tools/silvergear_spr_ledger.py research/silvergear/stock-app.disasm \
  --root 0x20000f8 --root 0x2054330 --root 0x2054d20 \
  --root 0x205265c --root 0x205c024 \
  --root 0x2027aec:0,e,1 --root 0x2027aec:1,10,1 \
  --tasks ../stock-backups/silvergear-3222882-20261102/analysis/stock-app-02000000.bin \
  --trace-call 0x2027aec --trace-call 0x2034d54 --trace-call 0x2034f00 --trace-call 0x20284c0 \
  --trace-call 0x202b328 --trace-call 0x202b54c --trace-call 0x202adf4 \
  --trace-call 0x202b410 --trace-call 0x202b1e8 --trace-call 0x202b2cc \
  --json > research/silvergear/printer-stock-init-ledger.json
```

Registration at `02000000` calls `02008710`, which stores descriptor pointers
in `020a9314[task]`. Scheduler `02008828` loads callbacks at descriptor offsets
8 (enter), 12 (leave), and 16 (poll); descriptor+4 is a mutable callback argument.
The ledger now follows all 28 nonzero callback candidates from ten descriptors:

| ID | Task | Enter | Leave | Poll |
| --- | --- | --- | --- | --- |
| 1 | Power Off | 0200ecdc | — | — |
| 2 | Video Recorde | 020109e8 | 020101d0 | 0200ffb4 |
| 3 | Photo Encode | 02008d1c | 02008994 | 02008a60 |
| 4 | Play Back | 0200ca44 | 0200c3c0 | 0200cb58 |
| 5 | Audio Player | 020171d4 | 02017274 | 0201702c |
| 11 | usb device | 0200fe50 | 0200fbdc | 0200fdb8 |
| 6 | Main menu | 02015b14 | 02015b00 | 02015aec |
| 7 | Game menu | 020163a8 | 02016394 | 02016380 |
| 8 | Game | 0201eba0 | 0201d5d0 | 0201e85c |
| 10 | setting menu | 0201f4d4 | 0201f53c | 0201f4c0 |

These are possible task transitions, not a claim that stock executes them all
at boot. The generated ledger explicitly reports 123 unresolved sites, including
69 indirect calls, depth limits, and a recursion cut. Memory loads other than
the explicitly verified six-word analog dispatch table and return values remain
unknown; a PC is visited once per call. Therefore this expands
coverage but does **not** establish a complete dynamic boot trace or prove the
absence of another enable. Calls invalidate condition flags; the earlier ledger
incorrectly retained a caller's known flag across calls.

The ledger also follows 13 board-device callback candidates from seven records
at `0209eccc` (registry pointer `0209ecbc`), plus six display-mode selectors at
`02082f08`. Together with five boot/printer roots and two explicit analog-helper
cases this produces 54 roots and 1,360 unique SPR write descriptions. The JSON
retains every unresolved site rather than claiming complete execution coverage.

A missed computed jump explains an important blind spot: `02027aec` loads a
six-entry table at `020840c4` and uses `l.jr r6` to dispatch. The old walker treated
all `l.jr` instructions as returns. Only `l.jr r9` is now a return; other known
constant targets are followed and unknown ones are reported. Selector0's enabled
case `02027b28` sets bit5 and trim bits4:0; selector1's `02027b50` sets bit12 and
trim bits11:7. Both write at `02027bec`. Inputs `(0,14,1)` and `(1,16,1)` produce
`002e` and `1800` in these fields. These explicit ledger roots illustrate the
helper semantics; they do not assert observed callback arguments. Stock camera
configuration calls this helper at `02050eb4` and `02050ed0` using profile data.

## Address-backed comparison

| Stage | Stock evidence | SDK / reference status |
| --- | --- | --- |
| General power | 0200011c → 02038800(1) → 0202b2cc; 02000128 → 0202b54c(0) | Existing Silvergear power setup follows the documented PM sequence; no new speculative PM bit is enabled. |
| Boot-time pad dependency | Photo enter → 02031b24; 02031bd0 configures PD2..15 as input with pull-down | Printer claim explicitly replaces PD13/PD15 route, direction, pulls and overrides. This does not prove electrical levels. |
| Analog dependency | 02027aec computed dispatch → 02027b28/02027b50 → 02027bec; camera calls 02050eb4/02050ed0 | Applying the camera setup `8868 & 1fff = 182e` changed the otherwise completed blank reference test into recognizable bars. Production now owns/restores that field. |
| NTC bias | 020033c0 configures PA6 input with pull-up; 02034928 selects ADC1 | Existing PA6 fix retained; averaging, invalid-range rejection and limit 65 remain. |
| Printer claim | 020541e0..02054318: PB6; PD0/PE10/PA3/PE11; PD15/PD13; PB5; PA9 | Production uses generic masked GPIO; reference independently performs the native SPR effects in stock pad order. Both preload safe output levels before enabling drive. |
| Electrical configuration | 02034d54 calls 02028b20/8a90/8874/8704/8c40/87a8 | GPIO route 8380+bank*4 set; 8100/8140 cleared; 8040 direction; 81c0/8200 overrides cleared; 8080/80c0 pulls. Final masked values match. Electrical roles of 8100/8140 are undocumented. |
| Idle levels | 020542dc..02054318 | PD15/PD13/PA9 low, PB5 high; motor phases and PB6 initially low. |
| UART / paper | 020526f8 clears selector 2 via 020284c0; PA7 input | Production and reference clear/restore 8340[9:5]; signal diagnostic does not read paper or use UART. |
| Supply | 02051800 clears four motor phases before PB6 high | Existing driver does the same and waits 200 ms. Today 2C1 remained 4.75 V with PB6 low and high. The earlier claimed gating was not reproduced; connection to head VH/VDD remains unverified. |
| PHY | 020541a0..dc resets d800/d804 | Preserved SDK USB. Earlier offline PHY/load comparisons were negative; no live PHY reset added. |
| Shift zero / one | 02051f2c clears PD13+PD15 then raises PD13; 02051f6c sets PD15 then clocks low/high | Reference uses these exact masked data operations. Production writes data then clocks low/high. Both sample the intended bit at the rising edge in the host model. |
| Latch / strobe | 0205c234..240: PB5 low/high, PA9 high, timer target | Production reuses the already shifted binary plane. Reference shifts the next identical plane while PA9 is high, then finishes it with PA9 low. |
| Shift/strobe overlap | 0205bca4; 0205bd38 threshold 190 early pairs; caller 0205c290..29c | Reference preserves overlap, with count/deadline checks during shifting. It may deassert earlier than stock's rate estimate to preserve the SDK bound. |
| Timing | 02051ff0 → 02029840: timer3 3809, reload ffff | Measured-rate check and 1112-count pulse limit retained. Reference resets the counter per pulse and uses relative counts; no absolute stock timing assumption is substituted for measurements. |
| Exit | 02052314 supply off; 0205c530 strobe off and motor idle | SDK keeps actuators as GPIO-low instead of restoring inherited active overrides. Paper is now checked between the 32 passes. |

The analog dependency is the demonstrated production correction. Native GPIO
initialization, PHY reset and shift/strobe overlap were insufficient by themselves
and are retained only in reference diagnostics. Production also avoids restoring
inherited actuator overrides, checks paper between synchronous passes, and defers
pending battery ADC conversions before a long half-row so a valid old conversion
does not become a spurious timeout. Battery freshness and thermal checks remain.

## RAM diagnostics and measurement record

Build from the SDK directory, keeping each example's objects separate:

```sh
make -j4 BOARD=silvergear-3222882 EXAMPLE=printer-signal-test BUILD=build/printer-bringup/signals APX_SPI_TRACE=0 APX_LAUNCH_TRACE_MODE=0
make -j4 BOARD=silvergear-3222882 EXAMPLE=printer-reference-test BUILD=build/printer-bringup/reference APX_SPI_TRACE=0 APX_LAUNCH_TRACE_MODE=0
make -j4 BOARD=silvergear-3222882 EXAMPLE=printer-test BUILD=build/printer-bringup/production APX_SPI_TRACE=0 APX_LAUNCH_TRACE_MODE=0
python3 tools/printer_patterns.py --output research/silvergear/printer-acceptance
```

The reference example alone selects `APX_PRINTER_REFERENCE=1`. It shares the
production interlocks, job API and cleanup, with independent native-SPR pad and
serial operations. It starts no job automatically. It is a bounded binary-raster
reference, not a complete stock grayscale implementation.

The signal example does not link the printing driver. It rejects CHECK/feed/
raster/photo jobs. Diagnostic-only APXD PRINTER command 12 accepts two LE u32s:
phase (0 idle, 1 supply-idle, 2 data-high, 3 clock-high, 4 latch-low, 5 strobe-high)
and operator heater-off evidence (0 or 1). Only phase 5 accepts/requires 1, and
it must begin from idle. All active phases expire after 12 seconds; USB loss,
suspend, cancel, shutdown and recovery return the outputs to idle. Motor phases
remain low. Normal production APXD behavior is unchanged.

From the workspace root, launch the signal manifest through the existing bench
with `--no-check`, `--setup <operator-setup.json>` and a new `--output` directory.
The setup records actual physical connections, battery state, probe contacts,
and earlier operator stock measurements. The bench archives firmware and logs.
Do not reuse a build/output directory while its bench is running.

First verify the complete measuring-tip path on an unpowered, broad known
contact. Resolve ground continuity and head VH/VDD at accessible PCB solder
points; do not adopt C1..C24 photograph candidates or the Seiko comparison as
an electrical pin identification. If reliable contact is unavailable, record
that limitation instead of repeating inconclusive fine-contact probing.

Start with the same terminal of **2C1** previously measured against the USB
shell. Send phases individually after positioning the probes:

```json
{"op":"diagnostic","phase":"idle"}
{"op":"measurement","point":"2C1, previously measured terminal","ground":"USB shell","voltage_v":0.0,"note":"replace with actual reading"}
{"op":"diagnostic","phase":"supply-idle"}
```

The example voltage above is an input-format illustration, **not a result**.
Take and record the actual supply-idle reading before expiry. Probe each mapped
signal with heater power off: idle versus data-high, clock-high, or latch-low.
Log every measurement with its exact point and ground. Requested phase, SDK
readback, operator voltage, connections and build identity remain separate.
Do not interpret an idle readback submitted after expiry as the earlier active
phase's voltage.

Only after electrically verifying the actual head VH is off, from idle:

```json
{"op":"diagnostic","phase":"strobe-high","heater_power_off_verified":true}
```

PB6-low readback is not enough to make that declaration. Record where and what
was measured. Compare stock supply voltages with the same probes, battery and
attached components; record that stock operates without USB, since USB changes
its behavior. A matching idle supply voltage or unchanged DC meter reading does
not establish a matching pulse waveform or rule out pulsed head current.

## Hardware results and acceptance

All runs archived exact firmware, build ID, raster hashes, SDK logs and operator
observations under `research/silvergear/printer-bringup-*`. Stock flash was never
written. Battery, printer, screen and both cameras were connected for the decisive
comparisons; only the documented offline tests deliberately removed USB.

| Run | Controlled difference / result |
| --- | --- |
| reference-01 | Native-style transfer; early battery ADC timeout; blank paper. Superseded by the completed comparisons below. |
| reference-02 | ADC handoff corrected; all 1,536 sparse strobes completed; blank. |
| reference-03 | Six byte-identical stock GPIO/serial leaves; all strobes completed; blank. |
| offline-01 | Battery only, exact stock PHY reset and native leaves; blank. |
| offline-02 | Additionally complete byte-equivalent stock GPIO initializer and its closed callees; blank. |
| analog-01 / analog-return-01 | Only additional change: `8868[12:0] 0190 → 182e`, restored afterward; **three recognizable bars**. |
| production-01 | Normal production GPIO/transfer, analog ownership fix, USB connected; **three recognizable bars**. |

The sparse 384×32 raster contains three 8-dot bars of heights 24/16/8 rows,
with at most 24 black dots per row (12 per half). Blank control generates no
strobes. Both use the existing stock-derived pulse energy at 100%; no energy
increase was needed. Production sparse output used 1,536 strobes, maximum242
counts, ADC1 about782/index24, measured timer about1.12MHz, and no battery ADC
timeouts. Native-reference maximum246 counts remains below the1112-count bound.

Production fault checks in `printer-bringup-production-20261107-01/result.json`:

- Cancellation after actual heating: cancelled at row23/512, 1,248 added pulses;
  heater/motor/PB6 all off.
- Lowered limit10 with actual index24: TEMPERATURE4, no added pulses or steps;
  all outputs off; default limit65 restored.
- Paper removed during sparse printing: PAPER6 at row47/512, all outputs off.
  Subsequent no-paper CHECK and feed both rejected with no pulses or steps.
- Paper reload restored a successful CHECK; BootROM recovery acknowledged and
  cleared/verified owned RAM with all outputs off.

Meter readings are operator reports against the USB shell: 2C1's same terminal
4.75 V idle and during PB6-high12s, unchanged afterward; 2C2 ends4.1 V and0 V.
These accessible components do not identify the actual head VH/VDD nets. Reliable
connector-pad continuity/static signal measurements were impractical with the
available probes, so the photograph pin labels remain provisional. Static
strobe-high was never run on hardware. An unchanged DC reading does not rule out
head current. No matched stock/SDK rail comparison is claimed.

Earlier timing-gap explanations (including the historical test23 comment in
the production driver) were hypotheses from tests before this analog fix; those
blank runs did not isolate timing as the cause. The current tight-pass transfer
is retained because it now passes the visible-print acceptance tests.

The historical `printer-analog-supply-test.json` tested this field before PA6
bias was fixed and recorded ADC1=0 with zero heater pulses. It excluded a cure
for that earlier ADC failure, **not** a dependency for actual heating.

Host verification covers native-ledger uncertainty, real GPIO effects feeding
a 384-bit rising-edge shift/latch model for both backends, alternating dot halves,
strobe ordering, all cleanup paths, timed signal phases and USB loss, battery ADC
handoff/freshness, printer protocol/bench tests, and ELF clock/manifest checks.
Cold-launch visual acceptance and camera-to-print results follow below.

### Three independent cold launches

`printer-bringup-cold-20261107-01`, `-02` and `-03` each started after the owner
reported full power-off with USB unplugged, followed by fresh forced BootROM.
Each used the identical production build `bf7db22520953000`, fresh power/DDR
bootstrap, blank32 then sparse32 then feed80. In all three the owner confirmed
"blank section then three recognizable bars". Every blank finished with zero
strobes, every sparse with1,536, maximum242counts, no errors or battery ADC
timeouts, and all three outputs off. Each ended in verified BootROM recovery.
See [separate operator acceptance records](printer-acceptance/hardware-acceptance-20261107.json)
and the individual run JSONs for build hashes, raster hashes and readback.

### Camera workflow

The initial demo `1d2432c6eeb225c1` exposed an incorrect physical button mapping;
its earlier camera-01 attempt never uploaded, and camera-02 recorded calibration
before a USB disconnect. Ordered owner presses gave PRINT134, SHUTTER512,
EXIT413, MODE614, decoded by the old table as SHUTTER/MODE/PRINT/EXIT. The
Silvergear board now maps those physical roles correctly; the resistor-ladder
centers and the printer driver are unchanged.

Corrected RAM build `69e6f7dd52f30fed`, archived in
`printer-bringup-camera-20261107-03`, captured a real ID39 sensor frame
(640×480, guards intact, no test-pattern fallback). The physical PRINT key froze
the image, and the second PRINT submitted the existing 384×288 dithered print.
The owner confirmed **a recognizable camera image**. The job completed all288
rows/576 motor steps,18,432 strobes, maximum397counts,100% energy, no printer
error and no battery ADC timeouts. Temperature index rose20→26, below limit65.
Camera INFO afterward showed live preview resumed, real sensor selected, frame
and preview guards intact, and zero capture timeouts. Its I2C error counter
includes expected profile-identification NACKs; it is not a print failure.

The board-only mapping correction changes the final production test build ID to
`ecca2c1489cb2d45`; the three cold-launch archives deliberately retain the exact
pre-mapping build `bf7db22520953000`. Their printer.c SHA256 is identical. Current
builds are under `build/printer-bringup/accepted-production` and
`accepted-camera`, both passing manifest/linker and clock checks. The complete
Silvergear model suite passed again after the mapping change; all178 host tests
passed, and the7 printer protocol/bench tests passed after adding read-only
`camera-info` bench support. No production APXD command was added.

The final camera run ended in verified BootROM recovery, with heater, motor and
PB6 off and SDK-owned RAM cleared and verified. No host bench remains active.

### Practical limits and next work

The first milestone is accepted: repeatable visible SDK printing, blank controls,
three independent cold launches, fault cleanup, and the real camera workflow.
Stock flash remains untouched. Grayscale fidelity and identification of the
physical analog rail controlled by8868 remain follow-up work; this result does
not claim scope measurements, calibrated head temperature, or a verified FFC
pinout. The complete dynamic boot trace is still limited by the explicitly
listed unresolved calls.


## Lengthwise photo layout follow-up

The owner clarified that stock places the photo's long edge along the paper feed.
The initially accepted demo used384×288 dots, putting the640-pixel camera width
across the head. The demo now samples the640×480 luma frame with a clockwise90°
rotation and scales it to384 dots across the head by512 rows along the feed,
preserving the full4:3 image. Floyd–Steinberg diffusion runs in output print order;
no intermediate full-frame buffer is allocated, and the packed raster grows from
13,824 to24,576 bytes. The source frame stays immutable.

A private image-conversion entry point shares the normal dither implementation;
the public printer API, APXD protocol and head/motor timing are unchanged.
Asymmetric, padded-source rotation and scaling cases pass in both printer models,
including raster capacity/guard checks. The complete Silvergear ASan/UBSan suite
and RAM build/manifest/clock checks pass. RAM demo build `b1ebbb5cd8912000` is under
`build/printer-bringup/lengthwise-camera`. In the archived run
`printer-bringup-lengthwise-20261107-01`, the owner confirmed the lengthwise
layout is correct. The job completed512/512 rows and1,024 motor steps with
32,768 strobes, maximum559counts (bound1112),100% energy, no printer error and
no battery ADC timeouts. Camera preview resumed with frame/preview guards intact;
heater, motor and PB6 read back off.
The direction of the90° rotation is clockwise; matching stock's handedness beyond
its owner-confirmed long-edge layout remains a separate visual comparison.


## Tear-off positioning follow-up

The owner clarified that the observed final motion sends a little more paper out,
placing the image near the tear edge; it is not a reverse retraction. The gray
wrapper loads80 at `020527f8` and passes it to `0205c024`; `0205c494..4a0`
doubles that count to160. The normal finishing loop `0205c4c4..500` calls the same
forward motor advance `0205bf08` as image printing. The stock motor record at
`020a3890` starts with phase stride2/direction1; the advance adds2 modulo8 at
`0205bf48..74`. This establishes the normal forward finishing count, not an
independent measurement of its time or mechanical travel. The SDK retains its
validated8ms step interval.

The initial camera-photo submission included that160-step tail; the final
calibrated count is256 as described below. The image and tail
remain one cancellable job; DONE and preview resume follow completion of the
feed. The heater stays low throughout the tail, phase progression continues
across the image/feed boundary, and paper loss aborts before another step.
Normal raster jobs and manual feed commands retain their previous counts.
Cancellation, failed temperature/paper preflight and other errors never start
an automatic finishing feed. Releasing the printer clears any pending tail.

The real-GPIO models verify162 total steps for a one-row blank photo, including
all four motor outputs in continuous forward order; real-heating cases verify
cancellation and paper loss during finishing, cleanup, and no tail leaking into
the next ordinary raster job. The complete Silvergear ASan/UBSan suite passes,
and the demo/production RAM manifests and clock checks pass. Camera build
`ab1198fc817c25de` is under `build/printer-bringup/tear-feed-camera`; the hardware
tear-edge result and final calibration are recorded below.


The160-step hardware trial in `printer-bringup-tear-feed-20261107-02` completed
all512 image rows plus160 finishing steps (1,184 total), with no printer errors,
32,768 heat pulses, maximum562counts, all outputs off and zero battery ADC
timeouts. The owner found the tear edge still needed approximately6mm more.
Three physical hold-PRINT operations each fed32 additional steps; the logs show
three successful32-step jobs with the heat count unchanged at32,768. The owner
identified those three feeds as the required additional distance.

The final board setting `APX_PRINTER_PHOTO_TEAR_STEPS=256` combines the stock
160-step base with that measured96-step correction. This is SDK tear-edge
calibration, **not** a claim that stock commands256 steps. Final models verify
258 total forward steps for a one-row photo, no additional heating, successful
cleanup and no tail for ordinary rasters. Final RAM builds are
`build/printer-bringup/tear-position-camera` (`56ac02fb3800f725`) and
`tear-position-production` (`2efd26bfd855bade`), with passing manifests, bounds
and clock checks. A final automatic256-step photo test follows in its run record.


The automatic256-step version (`56ac02fb3800f725`,
`printer-bringup-tear-position-20261107-01`) completed two512-row photos at1,280
steps each, with no errors. The owner then requested a further4mm buffer for
easier tearing, equivalent to two more32-step manual feeds. The final board
setting is therefore **320 forward finishing steps**: stock160 + calibrated96
+ requested64-step buffer. The256-step record remains a historical trial.

The320-step source passes the complete Silvergear model suite, including322
motor phases for a one-row photo, continuous forward order, zero heater activity
during finishing, cancellation/paper-loss cleanup and subsequent ordinary-raster
behavior. Camera build `9b77302680e33a5f` and production build `5cf61193084a3fa3`
pass RAM bounds, manifest and clock checks under
`build/printer-bringup/tear-buffer-camera` and `tear-buffer-production`.
The final4mm is an owner-requested tearing allowance. The initial load in
`printer-bringup-tear-buffer-20261107-01` later disconnected before a print
confirmation; the definitive hardware trial is recorded below.


### Final320-step tear-position acceptance

`printer-bringup-tear-buffer-20261107-02` loaded the exact RAM build
`9b77302680e33a5f` from a fresh forced BootROM connection. The owner confirmed
**a clean tear in the right spot without extra manual feeds**. Readback showed
all512 image rows completed and1,344 motor steps (1,024 image +320 finishing),
32,768 heat pulses, maximum576counts,100% energy, error0 and all printer
outputs off. Battery ADC timeouts remained0. Camera INFO afterward confirmed
real sensor ID39, resumed live preview and intact frame/preview guards.
The host observer detached while idle, leaving this tested RAM demo available;
stock flash remains untouched. No firmware change followed this acceptance.
