# Silvergear printer bring-up

Current status, **2026-11-07**: the normal SDK driver now produces the expected
three recognizable bars at 100% energy with USB connected. The missing dependency
was the camera analog setup `SPR8868[12:0] = 182e`; standalone BootROM starts
inherited `0190`. The driver now saves/applies/restores that field around printer
ownership, with outputs off before enabling/restoring it and the camera OFF.

See the initialization comparison and hardware acceptance record (`printer-initialization-20261107.md`, archived in `cleanup-archive-20261108/dated-bringup-reports-20261102-20261108.tar.gz`)
for address-backed evidence, controlled blank/printing comparisons, exact run
archives and validation status. The camera demo now rotates photos into384×512
dots with the long edge along paper feed, confirmed on paper by the owner. Photo
jobs also include a forward finishing feed; the current board setting is320
steps, including the owner-requested tear buffer, and a clean tear without
manual feeds was confirmed in the final hardware run. Actual
head supply contacts remain unverified.

The following entries are a chronological research record. Statements such as
"no motor step", "ADC1 unresolved" or "next test" describe their original stage
and are superseded by later results. Hardware runs use RAM firmware; stock
flash remains untouched. The physical setup of each run must be recorded
rather than inferred from an older test.

## Recorded observations

- Test 01 (`fe00debf395f025b`) observed PA7 high with no paper and low after
  insertion. Every sample ended with head/motor/power off and zero heat pulses
  and motor steps. Temperature ADC1 was zero, yielding TEMPERATURE error 4.
- The first example omitted `apx_power_domain_enable()`. Adding the existing
  setup enabled PM54 bit5, and the battery divider then read correctly:
  channel9 about 632, channel11 about 243, calibration800, about4190mV.
  That corrected the battery reading, not the zero ADC1 reading.
- A settling interval was added before ADC start and before accepting its
  result. The bootstrap has an explicit channel-settle delay; an immediately
  clear busy flag cannot prove a fresh conversion. Host models pass, but this
  change did not resolve ADC1 by itself.
- Test05 (`ded21969e55982c3`) performed sensor-only raw ADC probes. Channels
  0..11 respectively read approximately
  `265,0,647,1023,0,240,0,607,67,632,0,243`. Raw probes cannot arm feed or heat;
  subsequent normal jobs always return to channel1 and enforce the interlock.
- Stock timer3 mode `3809`, reload `ffff`, measured about **1.12MHz** against
  the independent 32.768kHz reference (`1119706..1124302Hz` across probes).
  The driver refuses motion/heating if its initial timer check fails.
- Paper readings occasionally varied while scanning. Stock printer setup at
  `020526f8` clears UART0 TX routing (selector2, SPR8340 mask3e0); this was
  absent from the draft driver. The source now saves/clears/restores that
  selector. A standalone masked trial did not resolve ADC1. The driver also
  now follows the stock PB5 latch-high idle level. Its new build still needs
  hardware acceptance.
- Bounded floating-input trials on already-undriven PB0/PB0..4 and PA6, and
  a trial of the previously tested camera analog-supply fields in8868, did
  not resolve ADC1. Each saved mask was restored/read back. These are negative
  diagnostic evidence, not a recovered thermistor pad mapping.
- The PCB point marked NTC is the **battery** thermistor according to the
  owner. It must not be presented as a verified printhead temperature point.
- `printer-stock-uart-02.bin` captured8255 bytes at460800baud (initial
  115200baud text is undecoded), SHA256
  `fc2da7759d38687adc4bb2d3152e1e616d3ae324aacdad6a4d43e017c1c82a32`.
  It contains normal stock startup, photo capture, a gap and UART
  reinitialization, but **no printer ADC value**. Gray Print success is
  owner-confirmed. The stock Gray wrapper clears the UART TX selector before
  claiming PA7, so the missing printer output cannot establish an ADC value.
  The screen was attached for this stock run.
- Test06 was cancelled while waiting for BootROM to allow cable setup without
  a timed window; it never uploaded or ran a payload.
- Test07 (`0d07565d2a81cabf`) tested the UART-route/latch corrections with the
  screen and rear camera disconnected, printer/front camera connected. Paper
  read present, battery about4190mV, but ADC1 remained0. No heat/motor steps.
  BootROM handoff and owned-RAM clearing were verified, then the device
  disappeared before Test08 could find it. The owner confirmed USB/power
  stayed connected. Test08 never uploaded its archived image.
- Test09 (`a5094ddb979390b7`, `printer-usb-power-test`) uploaded from a fresh
  entry and passed the exact APXD handshake. After the owner disconnected
  the camera's USB, the diagnostic performed CHECK and a raw ADC scan using
  battery power, saving results in its RAM log. ADC1 remained0 with the PM
  external-power indication0; raw channels0..11 read
  `264,0,600,1023,0,242,0,607,67,628,0,243`. Every result recorded head/motor/
  supply off, with zero motor steps or heat pulses. This rules out external
  USB power alone as the cause; it does not rule out USB PHY configuration.
  The UART receiver became disconnected during the physical step; it is not
  the source of these readings.
- USB subsequently returned with the same build and retained RAM log,
  establishing battery-only execution and subsequent USB communication for
  this run. `printer-battery-return-01.json` holds that log; Test10 attached
  without requesting a new job and observed sequence14,12 completed raw
  probes, timer1123103Hz, and all outputs off. Camera USB was initially absent
  when checked after reconnection, and disappeared again later while idle;
  the cause of these absences was not established. The owner explicitly
  clarified that their later kernel descriptor-read error-71 log came from
  **stock firmware**, whose USB-startup bug was already known. That log is
  not evidence of an SDK USB fault and must not be combined with the SDK
  connection observations as though it were the same firmware run.
- `printer-active-gpio-01.json` independently read back PB6 high during CHECK
  (`8004=62,8044=1f,8104=1f`), returning low afterward (`8004=22`). The heater
  and motor outputs stayed low, and ADC1 remained0. This is register/pin
  readback evidence, not a measured voltage at the printhead connector.
  A subsequent proposed PM54-bit6 supply trial stopped at USB discovery and
  executed no register writes.
- Test11 (`027a3fcb361e5e79`) initialized the verified LCD backend before
  CHECK, despite the physical panel being disconnected. LCD state reached
  READY(2), but ADC1 remained0. Thus that complete LCD initialization alone
  did not resolve the missing reading.
- Test12/13 (`021022a4c0d87331`) isolated the native stock USB-PHY reset at
  020541a0..020541dc. After physical USB removal, sensor-only CHECK ran with
  D800 readback342 and D804=0, versus normal readbacks2f2/1f. ADC1 remained0.
  SDK USB initialization restored the original readbacks and the same-build
  APXD handshake succeeded after reconnection. All printer outputs stayed off
  at completion, and no heating/motor request was made. D800 includes status
  bits beyond the written control value40; readback342 is not a failed literal
  comparison against40. The PHY reset alone did not resolve ADC1.
- The SDK-owned battery reference8868[15] was cleared for two CHECKs, then
  restored/read back (`printer-battery-reference-test.json`). ADC1 stayed0.
  A stock-derived weak-pullup trial on already-undriven SD0 pinsPA0..2 also
  left ADC1 at0; both pull registers were restored/read back, and no SD/SPI
  command was issued (`printer-sd-pullups-test.json`). These are negative
  setup-dependency tests, not a recovered temperature-pad assignment.
- Test15 (`6842d26f31fce7d1`) compared native stock ADC transaction timing
  without the cooperative settling delays. Battery sampling was shut down
  first to release ADC ownership, its reference bit was preserved during the
  measurement, and normal battery service was restored afterward. Priming
  channel3/9/11 returned1021/633/243 respectively; each first ADC1 result and
  subsequent20-sample mean/min/max was0, without timeout. The compiled
  `stock_read` SPR sequence was checked with native binutils. A stale-result
  workaround must not be used to authorize heating.
- The host bench's in-process `reload` completed verified BootROM handoff,
  owned-RAM clearing, DDR reuse, verified upload and exact APXD handshake for
  Test11->12 and Test14->15 without a fresh entry short. This eliminates the
  interactive gap that was present between Test07 and08.

The owner initially found no model marking; the November6 mechanism photos
now show the head codes reported as **6622B** and **6324B**. Neither code has
been tied to an authoritative part datasheet. The owner also confirmed stock
USB hot-plugging fails. The stock UART log
does not provide an ADC value, so neither stock printing success nor software
tables establish the physical temperature connection. No fallback value or
thermal-interlock bypass has been introduced.

Latest cleanup: Test15 shut down all printer outputs, acknowledged BootROM,
and cleared/read back all owned RAM reservations successfully. Test11,14,15
provide three verified SDK-to-loader recovery records during this session.
Normal stock startup after this latest group of checks has not yet been
owner-confirmed. The new photographs give a candidate temperature contact,
documented below. Unpowered continuity and resistance measurements are now
needed before tracing its mainboard bias and ADC connection.

Records are `printer-test-01..05/result.json`, their adjacent exact firmware
archives, `printer-adc-observation-01.json`, `printer-adc-pb0-input.json`,
`printer-adc-pb-inputs.json`, `printer-adc-pa6-input.json`,
`printer-analog-supply-test.json`, `printer-uart-route-test.json`, and
`printer-idle-latch-after-init.json`. The earlier idle-latch trial before init
was overwritten by initialization; only the after-init trial actually held
PB5 high during sampling. Test04/05 host benches were detached with a verified
printer shutdown; a later USB disappearance was the owner's deliberate stock
power cycle, not an observed firmware crash.

Test02 stopped at an SDK upload readback mismatch after re-running the DDR
bootstrap in a recovered session. No SDK takeover occurred. Later diagnostics
use either a fresh power-cycle/forced-ROM entry or immediately reuse DDR after
acknowledged SDK recovery and verified RAM clearing. The bench's `--recovered`
option requires that successful preceding recovery record and a recent file;
it does not invoke the DDR stage again. Test04 verified this upload/handshake
path. The earlier mismatch has not been attributed to flash changes; no flash
access was performed.

## Stock code and resources

Evidence uses the verified 4MiB backup SHA256
`4874af74051372363e8c36ff99e369657648eb353950b41fe2a5a00d35acdb9e`,
application SPI2200 mapped at02000000, Ghidra
`OpenRISC:LE:32:JT5295`, cross-checked with SDK binutils.

| Stock address | Recovered behavior |
| --- | --- |
| 02051800 | PB6 supply gate; first clears all motor phases |
| 02051dd8 / 02051dfc | PA9 heating strobe high/low |
| 02051e2c / 02051e50 | PB5 latch high/low |
| 02051f2c / 02051f6c | Shift zero/one on PD15 and clock PD13 |
| 02052060 | Motor phases PD0, PE10, PA3, PE11 |
| 020520f4 / 02052118 | ADC channel1; average20 and descending81-entry table |
| 020516c4 | ADC1 range classification; rejects the low range in Gray wrapper |
| 02052268 / 02052544 | Separate ADC-to-temperature table and preflight threshold65 |
| 02052510 / 0205be48 | PA7 paper read / stock debounce counters8,8,160 |
| 02054188 / 02054330 | Printer GPIO initialization / supply and paper-input setup |
| 0205265c | Gray wrapper; disables UART TX before paper/supply checks |
| 02053c60 | Dispatch using020b5922: 0 ->02052944, nonzero ->0205265c |
| 02052944 | Other print path; binary thresholds and longer timer3 sequencing |
| 0205c024 / 0205c584 | Gray transfer paths, alternating dots,32 weighted passes |
| 0205bc20 / 0205bca4 | Voltage/density-adjusted weights / shift next dot plane |
| 0205bf08 / 0205bf8c | Motor phase advancement / bounded stock feed sequence |
| 02029840 | Timer3 at9830=3809, reload9838, count9834 |

Stock02054188 also resets D800/D804. Those registers are used by the SDK's
USB PHY setup/recovery, so replaying that whole stock function would disturb
the live debugger. The separate offline PHY-reset test did not resolve ADC1;
the production printer driver reproduces its GPIO setup only.

The physical source/bias of ADC1 remains unresolved. A function's use by the
printer is not proof that a nearby NTC-labelled pad belongs to the head.
Ghidra functions reached through tables are being defined explicitly; generic
SPR decompilation can drop register side effects, so native instructions are
authoritative for pin/timing sequences.

`research/AnnotateSilvergearPrinter.java` verifies the complete application
hash and processor language before defining24 functions and annotating the
resource tables. It ran successfully against `stock-app-02000000.bin`; the
annotated program was saved. Its comments distinguish stock-code evidence
from the still-pending physical heater/motor acceptance.

`tools/extract_silvergear.py` generates the stock tables in
`boards/silvergear-3222882/printer.inc`. Relevant application offsets:
temperature thresholds a347c (81u16), battery gains a3898 (48u8), temperature
scales a38cc (84u8), initial weights a3928 (84u32), dot tables a3bf8/a3c58
(96u8 each), pass weights a3cb8 (32u16), motor phases8ea8c. Exact table hashes
and the verified backup identity are retained in board resources/build inputs.
Exhaustively combining valid temperature indices0..65, all dot counts,
passes and battery gains gives a maximum **1112 timer counts** per pulse.
The SDK rejects a larger computed pulse and has a separate reference-clock
timeout. This is a software bound, not an oscilloscope measurement of strobes.

## Mechanism photographs and thermistor hypothesis (November6)

Original photographs are preserved in `../printer-cam-printer-assembly-photos/`:

| File | SHA256 |
| --- | --- |
| printer-assembly-back.jpg | 4ba7b6ddc7b4a91f553037a4967f09c4b458e9c514d9ce86c2e44dc6ddbad572 |
| printer-assembly-front.jpg | 6e7c0b1f94cc4737618527fe8989968c12442cd5d03b04f0abb5cf3d662c2816 |
| printer-thermal-module-markings.jpg | 0d6adc8f8916d3fd75c5155dbe37de0938c0edce75cc7696d27acdb277b59471 |
| printer-thermal-module-markings-1.jpg | eec545614cec140714f3675692ac5a747d8f889d28c7a3756f3afda3c6716945 |

The back photograph shows24 conductors. Four branch toward the motor, three
toward the owner-identified IR paper sensor, and the rest enter the head
housing. Gold areas are conductors; dark slots are insulating separations.
Short rounded slots end where adjacent contacts merge into a common net;
they must not be counted as extra traces or unconnected contacts.

The grouping resembles the24-terminal arrangement in Seiko Instruments'
[LTP02-245-13 technical reference](https://download.mikroe.com/documents/datasheets/LTP02-245-13_datasheet.pdf),
Table4-2, printed page4-2 (PDF page43). That reference specifies TH at14,
ground9..11 and15..17, motor21..24 and paper sensor1..3; its section3.5.8
specifies a30kΩ thermistor at25°C with B=3950K. These are comparison data,
**not identification of this assembly or authorization to use its ratings**.
The head's physical sensor and its absolute resistance are still unmeasured.

For unambiguous photo discussion, local labels **C1..C24** count from the
motor-side edge. They are not verified PCB connector numbers. Reversing the
reference numbering gives this candidate map:

| Local photo contacts | Candidate role, pending continuity |
| --- | --- |
| C1..C4 | Motor phases |
| C5..C6, C19..C21 | Head drive supply groups |
| C7 | Latch |
| C8..C10, C14..C16 | Ground groups |
| C11 | Thermistor output |
| C12 | Heating strobe |
| C13 | Logic supply |
| C17, C18 | Clock, serial data |
| C22..C24 | Paper sensor connections |

Independently, all81 stock table entries at application offset`a347c`
(SPI`a567c`, RAM`020a347c`) match this model **exactly**, with zero count error:

```text
R(T)/Rpull = 3 * exp(3950 * (1/(T+273) - 1/298))
ADC(T) = round(1023 * R(T)/(Rpull + R(T))), T = 0..80
```

For example, indices0/25/65/80 give931/767/393/282. Thus the index represents
an intended0..80°C NTC model using a273K offset. It is not yet a physical
temperature calibration. The table determines **R25/Rpull=3**, not the
absolute resistor values:30kΩ/10kΩ is one matching example, not a measured
head resistance or mainboard pull-up. Using273.15 instead does not reproduce
all entries exactly.

Reproduce the SHA-guarded extraction, fit and photo overlay from the SDK root:

```sh
python3 research/silvergear/analyze_thermistor.py \
  ../stock-backups/silvergear-3222882-20261102/stock.bin \
  --output research/silvergear/printer-thermistor-fit.json \
  --photo ../printer-cam-printer-assembly-photos/printer-assembly-back.jpg \
  --guide research/silvergear/printer-contact-candidates.svg
rsvg-convert -o research/silvergear/printer-contact-candidates.png \
  research/silvergear/printer-contact-candidates.svg
```

The guide embeds an ordinary crop of the original photograph; it invents no
connections. Its circles identify insulated conductors, not exposed test
pads. Probe corresponding exposed contacts at the free ribbon end, without
piercing the coating. With battery, USB and UART disconnected and the ribbon
isolated from the mainboard, first check C9↔C15 on200Ω. If near zero, check
C11↔C9 on200kΩ and record the literal meter display and range. A finite
resistance decreasing when the metal backing is gently warmed by hand would
support an NTC identification. A different result first challenges the map;
stock printing success makes a premature defective-head conclusion unsound.

Owner's first unpowered measurement report (November6): C11↔C9 was about
100kΩ on200kΩ; one trial initially showed75kΩ and rose slowly to about130kΩ.
C9↔C15 "measured nothing" on200Ω, subsequently clarified as a lone1/OL
(over-range, not zero). No common ground or thermistor connection is confirmed
by these readings, and100kΩ must not be adopted as R25. Rising resistance could reflect
capacitance, changing probe contact, temperature or another path; its cause
is unresolved. The follow-up requested C8↔C9 on200Ω, since those adjacent
contacts appear to merge into the same copper strip. That checks probe contact
and contact numbering before relying on a connection through the head. The
owner again reported "nothing" and suspects their probes are too thick.
Consequently probe contact is unverified; neither result proves an open head,
broken ribbon, or incompatible pinout. Do not request repeated fine-contact
probing with those tips. The guide now marks physical mapping as paused until
reliable probe access is available. Its C9/C15 ground and C11 TH labels remain
candidates only; no mainboard pad has yet been electrically matched to C11.
Keep fingers off the exposed contacts/probe tips while reading high resistance.
The owner then clarified that metal-paperclip tip adapters were already
attached to the normal probes for these checks. Thus oversized stock tips are
not an established explanation. The next check is the complete adapted probe
path: touch both paperclip measuring tips together on200Ω for10seconds and
record stability/literal display; compare bare probe tips if it remains
over-range. This check touches no printer contact. Adapter resistance,
mechanical contact and the ribbon mapping all remain unverified.

No powered test, firmware upload, GPIO write, heater or motor operation was
performed for this photograph analysis. The invalid-ADC interlock remains.
November6 verification: all81 extracted thresholds matched the archived JSON;
an invalid backup was rejected by the SHA guard; Python compilation and
`git diff --check` passed. The updated annotation script passed its application
hash/language checks and ran against `stock-app-02000000.bin`, which was saved.
The rendered contact overlay was visually checked against the photograph.

## ADC1 ordering in stock (static finding, hardware result pending)

Found by reading `stock-app.disasm` with `tools/silvergear_spr_ledger.py` (a
constant-propagating SPR-write lister; `--root 0x2054330` reproduces the stock printer
GPIO setup, which matches the table above):

- All ADC reads of channel 1 go through `02034928` (select, 1 MHz divider, start, read),
  which matches `src/adc.c` and the `printer-adc-timing-test` `stock_read` sequence.
  Callers with channel 1: `0204e69c`, `020516c4`, `020520f4`, `020523e0`, `02052544`,
  `02052944` (twice), `02054d20` (three times).
- In the main print function `02054d20`, the **first** channel-1 read (`02054da0`,
  after `0204ed68`) comes **before** the printer GPIO init `02054188` (`02054dac`) and
  before the supply is raised by `02051800(1)` (`02054dc8`). The value goes to
  `02062fbc`, which has not been decoded; it may be the `printer_tm_adc()=%d` report.
- Every SDK test so far (`printer-test` CHECK, raw probes, `printer-adc-timing-test`,
  the PB0..4/PA6/PA0..2 pad trials) read ADC1 only **after** `apx_printer_init()` had
  already driven PB6, PD0, PE10, PA3, PE11, PD13, PD15, PB5 and PA9 as outputs. No
  test has read ADC1 with those pads untouched, which is stock's order. This does not
  show that one of those pads carries ADC1, only that the ordering was never tried.
- The stock release routine `02052314` clears the supply, then puts PD0 and PA3 back
  to inputs (`02034d54(3,1,1,0)`, `02034d54(0,8,1,1)`) and calls `0202c910(6,1)`
  (select ADC channel 6). Meaning not yet established.
- `0202c4ac`/`0202c588` (SPR 0x8870/0x8874/0x914x) are audio DAC setup, not ADC.

Test 16 (`printer-adc-pinscan-test`, build `f3a82dba11cd571e`, sensor-only, result in
`printer-test-16/result.json`; the `pinscan` lines were read afterwards with
`apxtool debug --port 1-1 logs` because the bench only saves the SDK log at start):
**negative.** ADC1 read mean 0, min 0, max 0 at the baseline, after each of the nine pads
was configured as an output in stock order (PB6 and PA9 held low, PB5 high), and after
releasing them again. Channel scans: baseline `240,0,458,1023,6,151,0,608,121,636,0,243`,
all pads configured `93,0,293,1023,6,78,0,608,122,635,0,243`. So pad configuration
order is not the cause. Caveat: the host CHECK runs first, so the "baseline" pads had
already been through one init/release; the true power-on pad state was not sampled.
Channels 1, 6 and 10 are exactly 0 and stable, while channels 0, 2, 5 and 8 move between
scans (floating inputs). A floating pad would not read a steady 0; a node with a
thermistor to ground and **no bias** would, so the sensor may be wired and its pull-up
unpowered. This is an inference, not a measurement.

Test 17 (same build, **screen, rear camera and printer all attached**, result in
`printer-test-17/result.json`, `pinscan` lines read with `apxtool debug --port 1-1 logs`):
**negative again.** ADC1 mean/min/max 0 at every step; baseline scan
`239,0,466,1023,6,151,0,608,121,637,0,244`, all pads configured
`93,0,298,1023,6,78,0,608,121,637,0,243`, essentially identical to test 16. The test
did not hang; it finished (`pinscan finished`) and the bench then idles waiting for
commands. Having the other peripherals connected is not the missing condition. The bench
now copies new SDK log text into `result.json` every 2 s (`silvergear_printer_test.py`).
Channels 0, 2 and 5 fall when the nine printer pads are driven low (240 -> 93, 466 ->
298, 151 -> 78), so those channels probably sit on or next to printer pads; channel 1 does
not move, and by the end of the release phase all nine pads were floating inputs.

Stock PM-chip use (survey of `202ace4` read / `202ad6c` write, register = write - 1):
the app touches PM registers 0x32, 0x34, 0x40, 0x42, 0x44, 0x46, 0x48, 0x50, 0x54, 0x58
and 0x5a. `apx_power_domain_enable()` replays 0x40/0x42/0x44/0x54/0x58/0x5a; 0x32, 0x34,
0x46, 0x48 and 0x50 are written by stock (`202adf4`, `202b410`, `202b1e8`, callers
`2038394`, `2038690`, `2038720`) but never by the SDK; their meaning is not decoded.
`202b98c(en)` sets/clears PM54 bit 5 (battery divider, 10 ms wait) and is called by the
battery read `20033c0` and the sleep path `200eb2c`; `202b9fc(en)` is the same shape for
PM54 bit 7 but nothing references it (dead code), so it is **not** evidence for a bias
switch. `202b504` returns PM_CONTROL (SPR 0x88e0) bit 21; the stock print entry
(`02054d20`) and `0204e69c` refuse to run when it is set (error 12/16). Correction
(owner, test 21): the whole stock firmware powers off as soon as USB is plugged in while it
is running, and cannot be powered on while plugged in, so this flag is stock's general
USB-detect, **not** a printing rule, and says nothing about printing from USB power.
Test 21 fed paper successfully on USB power. Test 09 already ran battery-only with ADC1 = 0.

Stock PM bootstrap decoded (`0202adf4`, called from boot function `02038394`; register
numbers below are the PM address, the stock call passes address + 1 to write):

1. `PM42 |= 0xe0` (the SDK does this as `write(0x43,0xe0)`).
2. Reads one PM byte at index 8 through `202abac`. If it is `0xaa`: **`PM54 = 0x01`** (a
   plain write) and the normal path ends.
3. Otherwise derives trims from the word at address `0x4fac`: with `r = (w & 0xffffff) >> 1`,
   `PM32/PM34 = (r & 0x3ff) ? (r & 0x3ff) - 2036 : 0xfa00` (low/high byte),
   `PM48 = (r & 0x1fc00) ? (r & 0x1fc00) >> 10 : 64`,
   `PM46 = 2 * ((r & 0x7e0000) ? (r & 0x7e0000) >> 17 : 32) + 1`, then **`PM54 |= 0x31`**
   (bits 0, 4 and 5).

On this camera the word at `0x4fac` reads `0xa955f288` (read with `apxtool debug read` from
the running SDK), which gives `PM32=0x50 PM34=0xf9 PM46=0x2b PM48=0x3e`. Either branch of
stock sets **PM54 bit 0**; the SDK never does (`apx_power_domain_enable()` only sets bit 5
and the PM trims 0x32/0x34/0x46/0x48 are never written). The meaning of PM54 bits 0 and 4
and of those trims is not known. Other stock helpers: `202b328(en)` toggles PM54 **bit 6**
(10 ms wait; callers `020032fc`, `02038824`, not part of boot), `202b410` toggles PM46
bits 1/2, `202b398`/`202b494` PM5a bits 0/1 and 3, `202b1e8` writes PM50..53 (callers
`02038690`, `02038720`).

Test 18 (`EXAMPLE=printer-adc-pm-test`, build `d5e8fd73d420ec3e`, result in
`printer-test-18/result.json`): dumps PM 0x30..0x5a, prints the trims stock would write,
reads ADC1 at baseline, replays stock's PA6 setup (input + pull-up, from the battery read
`020033c0`), then sets PM54 bits one at a time, restoring PM54 after each. No trim register
was written, PM54 bit 6 was never touched, no heater/motor/supply pad was driven.

**Root cause found: PA6 is the head thermistor input and its internal pull-up is the
divider bias.** Results:

| Step | ADC1 |
| --- | --- |
| baseline (PA6 not configured) | 0 |
| **PA6 input + pull-up** | **782** (min 782, max 783) |
| PM54 \|= 0x10 (PM54 became 0x31) | 0 |
| PM54 \|= 0x11 | 0 |

782 is about 23 C on the stock table (767 is 25 C), a plausible room temperature. With
the pull-up on, channel 0 also moved (231 -> 544), so channel 0 is probably a neighbouring
pad. Every earlier test left PA6 floating or untouched, including the old "PA6 floating
input" trial, which is why ADC1 stayed 0. This also fits the exact-zero readings
(thermistor to ground, no bias). The inferred pull-up of about 10 kOhm matches the
R25/Rpull = 3 fit of the stock table for a 30 kOhm head thermistor, but neither value is
measured.

Not the cause: PM54 was already 0x21 (bit 0 already set), setting bit 4 changed nothing,
and the PM trim registers 0x32/0x34/0x46/0x48 read 0x00/0x00/0x00/0x40 (stock's trim path
did not run on this unit: the PM byte at index 8 is probably the 0xaa marker), so none of it
affects ADC1. PM54 bit 6 is the **LCD backlight** on this board (`board.h`), which is what
stock's `202b328` toggles; PM54 returned to 0x21 after the test.

Fix: `src/printer.c` now owns PA6 (mask 0x2c8 in group 0) and configures it as an input with
pull-up in `apx_printer_init()`; release restores it from the saved registers. The thermistor
interlock is unchanged. `tests/printer_model.c` asserts the PA6 configuration; all
Silvergear host models pass under ASan/UBSan. Hardware acceptance of the fixed driver:

Test 19 (`printer-test` build `6c424362e17befbf`, `printer-test-19/result.json`, sensor-only
CHECK, USB powered): **temperature path accepted.** `temperature_adc=782`,
`temperature_index=24`, `temperature_valid=1`, timer 1,122,104 Hz valid, no heat pulse,
no motor step, all outputs off afterwards. The CHECK still ended in state 5 / error 6
(`APX_PRINTER_PAPER`) with `paper_signal=1`: PA7 high, i.e. no paper sensed, which is
the paper interlock working as designed (a CHECK without paper reports PAPER). Whether
paper was loaded for this run is not recorded; if it was, the paper input needs
investigating (polarity or the UART route on PA7, see the earlier tests).
Test 20 (same build, `printer-test-20/result.json`, paper loaded, screen and rear camera
attached, USB powered): **CHECK passes.** State 4 DONE, error 0, `paper_signal=0`,
`paper_valid=1`, `temperature_adc=781` (index 24), `temperature_valid=1`, timer
1,123,103 Hz valid, `completed=1`, no heat pulse, no motor step, all outputs off at the end.
So the paper input behaves as expected (high without paper in test 19, low with paper
here). Next: a short feed (first motor motion), then sparse raster.

Test 21 (same build, `printer-test-21/result.json`, USB powered, paper loaded): **first SDK
motor motion works.** After a CHECK (ADC1 783, valid), `feed 16` gave state 3 / `motor_on=1`
(6 steps at the first sample, 0.25 s after the command), then state 4 DONE with
`steps=16`, 0.37 s after the command; `heat_pulses=0`, `head_on=0`, `motor_on=0` and
`power_on=0` afterwards. The owner confirmed the paper was pulled in. So the stock motor
phase table, the PD0/PE10/PA3/PE11 mapping and the 263-tick (8 ms) step spacing drive the
mechanism, and it runs on USB power. The `error` field in that `result.json` is the bench's
normal exit after stdin closed, not a fault. Distance per step and direction relative to the
print face are not recorded yet.

Expected pulse lengths for the first heater test, computed with the driver's formula and the
stock tables at index 24 (`research/silvergear/raster-sparse-384x8.bin`, 16 dots per row):
about 0.22 ms maximum pulse and 3.3 ms total heating per half-row at 4.2 V (0.34 ms and 5.0 ms
at 3.7 V); the largest case the driver can print (192 dots per half-row) gives 0.48 ms
maximum pulse and 7.2 ms per half-row at 4.2 V. All are far below the 1112-count cap. These
are computed values, not measurements.

Test 22 (same build, `printer-test-22/result.json`, USB powered, paper loaded, temperature
limit set to index 35, `raster-sparse-384x8.bin`): **first SDK heater run completed.** State 4
DONE, error 0, `steps=16`, `heat_pulses=256` (8 rows x 32 passes; only one half-row of each
row has dots because all dots sit on even columns), `max_heat_count=250` (exactly the
predicted value at 4210 mV, index 24), temperature index stayed 24 (ADC 780-781),
`head_on=0 motor_on=0 power_on=0` afterwards, job time about 0.64 s. The owner saw the
printer print "very little", with the paper barely moving. That matches the geometry: if the
head is 48 mm wide (assumed, 2-inch class, unmeasured) and rows are square, one step is
0.0625 mm, so 16 steps is only about 1 mm and the dashes are about 1 mm tall. Whether marks
appeared on the paper has not been reported. Timing note: 40 ms per half-row because the
driver re-shifts all 384 bits on every one of the 32 passes (only the strobe length changes
between passes for a one-bit raster); a speed-up is possible later by latching once.
Test 23 (`printer-test-23/result.json`, build `6c424362e17befbf`, USB powered, limit index 35):
`feed 320` moved the paper about **20 mm, as predicted (0.0625 mm per step, 16 steps per
mm)**; the bench saw 320 steps in about 2.8 s (8 ms per step). Then `raster-ramp-384x32.bin`
completed (state 4, error 0, 64 steps, `heat_pulses=2048`, `max_heat_count=320`, temperature
index 24 throughout, battery 4216 -> 4178 mV between half-rows) but **no marks appeared**.
All strobe pulses were delivered, so the strobe path ran; the energy was applied badly.

Analysis (static, stock `0205c024` / `0205bca4` / `02051f2c` / `02051f6c`): stock per pass does
latch low/high, **strobe high**, reads the timer to set `target = now + count`, shifts the
next plane (first about 1.5 x count bit pairs), busy-waits for `target`, **strobe low**, then
shifts the rest. Passes therefore run back to back and the head sees nearly continuous
heating (a pair of bits costs about 0.6 us in stock, so the strobe-on time is about the
count). The SDK instead shifted all 384 bits before every pass at about 1 ms per plane
(measured 40 ms per half-row), so its 0.2-0.3 ms pulses were separated by about 1 ms of
cooling: the same total energy, far lower peak dot temperature. This is the leading
explanation for the blank output, not a proven one. (The wrong-polarity and wrong-unit
alternatives were checked against the stock strobe helpers: `02051dd8` sets PA9 high at the
start of each pass and `02051dfc` clears it, the same as the SDK.)

Change in `src/printer.c`: one-bit rasters shift and count dots once per half-row; the 32
passes then run in a tight loop (latch pulse, strobe for the weighted count), with one poll
phase before the next half-row so USB is still serviced. The 1112-count cap, the
per-half-row temperature sample and all interlocks are unchanged. Heating per half-row is
unchanged in total; at 4.2 V and index 24, 64 dots give 4.2 ms and 96 dots 4.7 ms in total
(maximum pulse 315 and 359 counts). `tests/printer_model.c` was adapted (one shift per
half-row; the timer-fault test now uses an all-black bitmap); all Silvergear models pass.
Test card: `raster-testcard-384x160.bin` / `.png` (dashed frame, "PRINT OK", squares and
slashes; at most 98 dots in any half-row). Build `bad91fc71abc2707`, hardware pending.

Test 24 (`printer-test-24/result.json`, build `bad91fc71abc2707`, USB powered, limit index 35,
`raster-testcard-384x160.bin`): job completed (state 4, error 0, 320 steps, `heat_pulses=10240`
= 320 half-rows x 32 passes, `max_heat_count=369`, temperature index 24 throughout, battery
4210 -> 4184 mV, about 10 ms per half-row, 3.3 s in total) but **the paper stayed blank.**
Back-to-back passes did not change the result, so the gap between pulses was not the (only)
cause. Checked afterwards and ruled out: timer unit (stock's gray path also uses timer 3 mode
`0x3809` via `2051ff0` -> `2029840`, and reads counter SPR `0x9834` through `2029824`; mode
`0x42` is only used by the dot path `02052944`), strobe polarity and latch order (same as stock),
data polarity (stock shifts "one" for dark pixels), PM-chip helpers (none reachable from the print
code: `202adf4/202b54c/202b2cc/202b494` run at boot only, `202b328` is the LCD backlight), and the GPIO
pad configuration (same register writes as stock `2034d54`). Remaining suspects, in the order they
can be separated: (1) USB power: every heater job so far ran on USB, which stock never does (it powers
off on USB); the head supply path may collapse or be isolated while USB is present; (2) pulse energy
too low: stock's strobe stays high while the next plane is shifted, so its real on-time can exceed the
timer count by an unknown factor (up to about 5x if an SPR access costs about 40 cycles, as the SDK's
own shift loop suggests; about 1x if stock's shift is as fast as its 1.5-pairs-per-count constant
implies); (3) head supply or wiring (PB6 may not be the head supply gate) or data/clock not reaching
the head. Neither (1) nor (2) can be excluded from the data we have.

Diagnostic added to separate them: `apx_printer_set_energy(100..400)` (APXD command 11, bench op
`{"op":"energy","percent":N}`, `energy_percent` in INFO; each pulse is clamped to 1112 counts, so
at 64 dots and 4.2 V a 400 % request is limited to 1112) and
`EXAMPLE=printer-battery-print-test` (build `d782bf0088da9df8`): once USB power has been absent for
3 s it prints three bands (128 px wide, 24 rows, at 100 %, 200 % and 400 % energy, limit index 35) on
battery power only and logs state, pulses and battery voltage per band. Reading the outcome: marks on
every band = USB power was the problem; marks only on the 200/400 % bands = energy (scale the
stock-derived pulses); nothing on any band = the head supply or the data path, not power source or
energy.

Test 25 (`printer-battery-print-test`, build `d782bf0088da9df8`, `printer-test-25/result.json`
holds only the pre-unplug part; the rest is in the SDK log ring, read with
`apxtool debug --port 1-1 logs` after the replug): USB power was lost for 3 s
(`battery=4196 mV`), then all three bands ran on **battery only** and completed: band 1 (100 %)
state 4, error 0, 64 steps, 1536 heat pulses, max pulse 320; band 2 (200 %) 3072 pulses, max 640;
band 3 (400 %) 4608 pulses, max 1112 (clamped); temperature index 24 throughout; battery 4150 then
4132 mV (sampled between half-rows, so it cannot show the load during a pulse). **No marks on any
band.** This rules out USB power and pulse energy (up to 1112 counts, about 1 ms, 32 passes per
half-row) as the cause. What remains is the head supply (PB6 or another gate) or the data/clock/latch
path to the head. The owner could not read the log after the replug, although it was readable
afterwards on `--port 1-1` (the device enumerates again as `JT5295 SDK Debug`).

Next diagnostic, `EXAMPLE=printer-load-probe-test` (build `93825b584f4da847`): checks whether the head
draws any current. Per trial it shifts a plane with D dots into the head, pulses the latch, then
holds the strobe high for 2 ms while reading the battery divider (ADC9) continuously, and compares
with the idle reading. Trials (PB6 high unless noted): no dots, 96 dots without strobe (control),
32, 96 and 192 dots, 96 dots with PB6 low, 96 dots again. It runs once on USB and once on battery.
No motor movement, strobe never above 2 ms, forced low on every path. A sag only for dots + strobe
with PB6 high means the head is powered and latched (then the marks problem is thermal/paper); no sag
in any trial means no load current: no head supply, or no dots reaching the head.

Test 26 (`printer-load-probe-test`, build `93825b584f4da847`, USB powered with a battery
fitted; the owner stayed on USB, so only the USB run happened; result in the SDK log ring, read
with `apxtool debug --port 1-1 logs`): ADC9 read 635-637 idle and 634-637 during the 2 ms strobe in
**every** trial: no dots, 96 dots without strobe, 32, 96 and 192 dots with strobe, 96 dots with PB6
low, and 96 dots again (160 conversions per trial, n=160). There is no sag. On USB this is
**inconclusive**, not proof of "no head current": with USB present the charger can hold the battery
rail and the divider node may be filtered, so a load could be masked. The battery-only run was left
armed (the firmware runs it 3 s after USB power is lost); it is the discriminating measurement. The
bench ended with a USB I/O error during the probe (blocking sections), which is harmless.

Test 26 battery run (owner unplugged USB later; log pasted): on **battery only** ADC9 stayed at
633-634 idle and 633 during the strobe in every trial (n about 160 each), including 192 dots latched,
and with PB6 low; the second USB run again read 636-637 throughout. So there is no measurable sag
on USB or on battery. That means either the head draws no current or ADC9 cannot see loads from
where it is; a positive control is needed before concluding anything. (Observation to use:
the driver's `battery_mv` moved 4196 -> 4150 -> 4132 mV across the three bands of test 25, so the
battery reading does respond to load activity at the tens-of-mV level, but those samples were taken
between half-rows, not during pulses.)

Probe v2 (`printer-load-probe-test`, build `1437463c2c980878`): adds (a) the motor as a known load,
100 ms hold with ADC9 sampled throughout; (b) 160 motor steps with PB6 high, then 160 with PB6 low,
logged with ADC9 min/mean (visual check: if the paper moves in both runs, PB6 does not gate the
motor supply, so it need not gate the head supply either); (c) one 5 ms trial at 192 dots; (d) on the
battery run only, stock's USB-PHY register state (`0xd800`/`0xd804`, reset by stock `02054188` before
every print and never applied in a heater test) with the 192-dot head trial and the motor hold
repeated, then USB restored through `apx_usb_init()` as in printer-phy-test.

Test 27 (probe v2, build `1437463c2c980878`, owner's pasted log; USB run then battery run):
- **Positive control works.** Motor hold (PB6 high): ADC9 idle 634 -> min 618, mean 621 on
  battery (about 13 counts, 85 mV); 160 steps with PB6 high: min 616, mean 620; on USB min 622,
  mean 631-632.
- **PB6 gates the motor supply.** 160 steps with PB6 low: ADC9 mean 632 against idle 632
  (battery) and 635 against 635 (USB), i.e. no motor current. (Whether the paper moved in that run
  was not reported.)
- **The head draws no measurable current.** With PB6 high, a plane of 32, 96 or 192 dots latched
  and the strobe high for 2 ms (and for 5 ms at 192 dots), ADC9 stayed at idle (633 on battery,
  635 on USB, min within 1 count) in every trial, which is far less than the 13-count motor sag
  although a head load of a few hundred mA or more would be larger. The stock USB-PHY register state
  (`0xd800`/`0xd804` as in stock `02054188`, from `D800=3f2 D804=1f`) changed nothing: 192 dots,
  strobe 2 ms: min 632 against idle 633.
- Static findings (no missed step): `2051684(500)` is a busy loop of 10,000 iterations (a
  fraction of a millisecond), not 500 ms, so stock does not wait longer than the SDK after raising
  PB6; `2051768`/`20516c4` only band the ADC1 temperature; `2003600` is a getter; `205265c` configures
  PA3, PE11, PE10, PA7, PB6 and clears the UART route exactly as the SDK does; the unreferenced PM
  helpers are dead code; PD1 (`2003478`) is a high/low/high reset pulse; `202b54c` enable matches
  `apx_power_domain_enable()`. The stock print path also calls an authorization check (`2060ff0`;
  "OpenImage Credit Check Failed!" is in the stock boot log yet stock printing worked).
- Open question: **does stock still print on this unit?** The owner took the printer assembly apart
  for photographs on 2026-11-06 (see the photos), and every SDK heater test since then printed nothing,
  while "normal stock startup after this latest group of checks has not yet been owner-confirmed". A
  damaged or unseated head flex or heater contact would look exactly like this (motor, thermistor and
  paper sensor share the same ribbon and still work). Until stock is shown to print now, the missing step
  could be hardware rather than software.

Owner confirmation (after test 27): **stock still prints fine on this unit**, so the head and
ribbon are healthy and the SDK is missing a supply or enable. Further static results: the only
GPIO pads stock configures outside the printer set are PC4 (backlight handler `020032fc`), PD1 (a
high/low/high reset pulse, `02003478`) and the button/LED pads, so no unknown pad enable exists;
`02038690`/`02038394` write the ASCII tag `ACTR`/`CTR` to PM50..53 and read a UUID: chip
authorization, not power.

Leading hypothesis now: **PM54 bit 6 (the LCD backlight) also supplies the head.** Stock prints with
the screen on, so that bit is always set while it prints, and no stock print code touches it. In
every SDK test it was clear (PM54 read 0x21). A backlight boost that shares the head's supply would
produce exactly what was seen: motor works (VBAT through PB6), head draws no current, nothing
missing from the print path. This is an inference, not proven; the earlier notes had already proposed
this bit, but that trial never ran. Changes: `src/printer.c` now sets PM54 bit 6 in
`apx_printer_init()` when it is clear (stock waits 10 ms after writing it) and clears it in the
release only if the driver set it; the bit as read is exposed as `backlight_bit` in INFO; the host
model checks both cases. `EXAMPLE=printer-load-probe-test` v3 (build `1f14e004945309e9`) repeats the
head trials with PM54 bit 6 forced clear, then set, with the motor hold as a control. `printer-test`
with this driver is build `2e9de8351a209f81`.

Tests 29 and 30 (owner): both made the screen backlight light for about a second (so PM54 bit 6 is
the backlight and the write works) and printed nothing. Test 29 (`printer-test` build
`2e9de8351a209f81`, driver setting PM54 bit 6) is **invalid as a print test**: the card raster was
accepted at 28.97 s and the bench closed stdin and shut the job down 40 ms later (steps 0), because
the fixed `sleep` delays assumed a startup time that varies with the forced-BootROM entry; the first
`limit` command was also rejected BUSY during the startup CHECK. The bench now has `{"op":"wait"}`
(hold commands until the printer is idle; also before the first command) and
`{"op":"sleep","seconds":N}`. Test 30 (probe v3, build `1f14e004945309e9`, USB run, `PM54=61`
with bit 6 set): ADC9 stayed at 635 in every head trial (0/32/96/192 dots, strobe 2 ms, 192 dots
5 ms), same as with bit 6 clear (`PM54=21`); the motor hold control still dipped (min 627 mean 630).
**So PM54 bit 6 is not the head supply.** The driver change was reverted (the driver no longer
touches PM54). The camera was no longer in SDK mode afterwards, so the rest of the v3 log was not
retrievable.

Test 31 (`printer-pad-loopback-test`, build `0223adff56185b1b`, PB6 low throughout; the new bench
`wait`/`sleep` ops worked and the log reached `result.json`): ADC channel 6 follows PD0 exactly
(0 -> 1023), so bank D GPIO writes drive their pins. No ADC channel follows PA9 or PB5 or PD13/PD15 or
PE10, PA3, PE11, so those pads cannot be verified this way. The apparent PA9 "follow" on channels 0, 2
and 5 is a decaying floating-input artifact (channel 0 reads 601, 270, 231, 227 over successive pads),
not coupling. ADC1 (PA6) read 776 throughout. Static diff: stock's boot code writes 246 distinct SPR
addresses; the ones the SDK never touches are interrupt/timer (`0x18`, `0x18xx`, `0x48xx`), PM bus
(`0x88e4`, `0x88ec`), DMA engines (`0x89xx`), audio (`0x914x`), LCD (`0xb8xx`), camera/ISP (`0xc0xx`,
`0xc84x`), `0x92xx`, `0x988x`, `0xa0xx`; nothing there relates to GPIO or the head. Board photos
(`printer-cam-pcb-photos`): the SoC side carries two DC-DC converters, each with its own inductor
(converter 1: SOT-23-6 marked `J5OA491` or similar, bottom left; converter 2: SOT-23-6 marked `AMB` with
a Schottky `SL`, lower middle-right), and the printer FFC connector has no driver or level shifter
beside it (the head signals look direct from the SoC). `research/silvergear/printer-supply-probe-points.png`
labels the probe points (1L/1C1/1C2/1IC and 2L/2C1/2C2/2IC/2D).

Next test, `EXAMPLE=printer-supply-cycle-test` (build `0bc9cb57e417dc83`): cycles PB6 and PM54 bit 6
through four states every 8 s (PB6/bit6 = 0/0, 1/0, 1/1, 0/1) for 8 cycles so the owner can read the DC
voltage of one point per cycle with a multimeter and see which converter PB6 switches and what its
output voltage is. No strobe, latch, clock, data or motor pad is driven.

Owner observation while starting `printer-supply-cycle-test`, before any measurement: audible coil
whine that becomes noticeable and then goes quiet. In that firmware only PB6 and PM54 bit 6 change, so
a DC-DC converter inductor is being switched by one of them (start-up charging is typically the audible
part; an unloaded converter then drops into burst mode and is quiet). Which state and which inductor
is not yet known. Follow-up (owner): the whine appeared at **state 1 (PB6 high, PM54 bit 6 low)**.
So **PB6 starts a DC-DC converter** (the backlight bit is not needed), consistent with the motor
supply gating seen in test 27; whether the head shares that rail, and its voltage, are still
unmeasured. Which inductor (1L or 2L) is not yet known. The owner also felt the stepper motor warm
during runs. Earlier data argues against a standing DC load in the idle states: ADC9 idled at 633-635
with PB6 high and the motor pads low (tests 26/27) against 618-622 during a motor hold, so a coil
carrying a motor-sized current would have shown. The cycle firmware did not configure the motor pads
itself (it relied on the startup CHECK leaving them low), so it now sets PD0/PE10/PE11/PA3, PA9,
PD13/PD15 low and PB5 high explicitly (build `33c80c5a3837f5de`). Still to determine: which firmware and
which state the warmth appeared in, and how warm.

Owner follow-up: the motor heated slowly during the runs of `printer-supply-cycle-test` build
`0bc9cb57e417dc83` until it could not be touched for more than a few seconds, and the coil whine was
there; **with the later build `33c80c5a3837f5de` (explicit idle pads) neither the whine nor the heat
occurred.** Finding in the firmware: neither build calls `apx_printer_poll()`, so the bench's startup
CHECK never completed and the driver stayed claimed (state SAMPLING) with the pads as `apx_printer_init()`
sets them, for the whole run; the pad states of the two builds should then be electrically the same, so the
difference is not explained. Whether the screen and rear camera were attached in the first run is
unknown. The whine therefore cannot be attributed to PB6 alone (the earlier inference that PB6 starts a
converter is weakened: the later build also cycles PB6 and is silent). `EXAMPLE=printer-heat-watch-test`
(build `72ab32a208811c6a`, run the bench with `--observe`) runs the cycle in the stuck-CHECK condition
(phase A) and then with explicit idle pads (phase B), reading ADC9 (load sag) and ADC1 (head thermistor)
every 250 ms and aborting if ADC1 falls 40 counts; it logs per-state minimum and mean ADC9 and the latest
ADC1 against baselines, to show whether a load exists, whether it warms the head, and in which phase.

Test 37 (`printer-heat-watch-test` build `72ab32a208811c6a`) was **invalid**: the bench was not started
with `--observe`, so it issued its own CHECK (the firmware's `apx_printer_check()` returned 2, BUSY);
the firmware then blocked in a 1 s `apx_delay_ms()` without servicing USB, the bench's next request
failed with APXD status 3 and the bench exited with a printer shutdown, which released the driver
mid-cycle (PA6 back to floating, ADC1 = 0), and the ADC1-fall abort misread that as head warming. The log
read afterwards with `apxtool debug --port 1-1 logs` shows only the state 0 line (PB6 low) and the abort,
so no PB6-high state ever ran and "no heat, no whine" in that run is expected. Fixed in build
`23d9cf4bf1d4f1c0` (USB serviced during the wait; ADC1 readings below 100 ignored). Owner detail
for the original heating run (`0bc9cb57e417dc83`): the **screen** (not the rear camera) was connected at
one point to see which state was running, then disconnected again, so the screen's presence is a
candidate variable (backlight load and the shared LCD/printer pads PD13/PD15 are both plausible).
Test 38 (`printer-test-38`, heat-watch build `23d9cf4bf1d4f1c0`, `--no-check`): the owner felt no motor
heat and heard no whine. The bench died at 20.2 s with APXD status 3 (= `APXD_UNSUPPORTED`): that is its
periodic `battery.info()` request, which fails once the firmware has shut the battery service down; the
bench now tolerates it. The firmware itself ran to completion and the log (read with `apxtool debug
--port 1-1 logs`) shows: phase A (the bench's shutdown had released the driver, so ADC1 read 0 and the pads
were in their post-release state rather than the stuck-CHECK state): ADC9 min 633-636 in all states
(baseline 636; 2-3 counts lower with the backlight bit set, i.e. the screen's backlight load); phase B
(explicit idle pads): ADC1 768, ADC9 633-634 in every state, no sag. **No load or warming was reproduced.**
The heat and whine seen with build `0bc9cb57e417dc83` are therefore unexplained and treated as a one-off;
the exact stuck-CHECK-with-bench-connected condition was not reproduced because the bench crashed first.
Test 39 (`printer-test-39`, heat-watch `23d9cf4bf1d4f1c0`, `--no-check`; the bench survived this time and
logged to `result.json` until the device disappeared at 93.8 s): phase A (stuck CHECK, ADC1 771) and phase
B (idle pads, ADC1 771): ADC9 633-636 in every state, ADC1 unchanged, no heat, no whine (owner). The
0bc9 heating event is not reproducible; dropped. Next: multimeter readings with
`printer-supply-cycle-test` rebuilt as a two-state cycle (PB6 low / high, 12 s each, 12 cycles, backlight
bit untouched, build `18bed017b60f2f5a`), probe points in `printer-supply-probe-points.png`.
Test 40 (`printer-supply-cycle-test` build `18bed017b60f2f5a`, owner's multimeter, ground = USB shell):
no change was seen at most points; **2C1 (output cap of converter 2, the `AMB` chip with the Schottky)
switched between 4.75 V and near zero with the cycle.** So converter 2 is a roughly 5 V rail that follows
the supply cycle (assumed: 4.75 V in the PB6-high state; to be confirmed); it is the likely motor/head
supply. Converter 1 and the battery pad did not change. The head still draws no measurable current from
the battery side (ADC9), so the next test looks at the rail itself: `EXAMPLE=printer-vh-burst-test`
(build `d5aa79378a326e96`) per 10 s phase: PB6 low; PB6 high idle; PB6 high with 2 ms strobes every 8 ms and
no data latched (control); the same with 96 dots latched, x3, ADC1 abort at -40 counts. A head that conducts
should pull the 2C1 reading down in the last phase only.
The bench's `--observe` is only valid with `--attach` (firmware already running); a fresh launch that
must not start the automatic CHECK now uses the new `--no-check` option instead.

Distance per step was unmeasured (now measured, see test 23 above): use `feed 320` (about 20 mm if the 0.0625 mm
estimate holds) and a ruler. Next test: `raster-ramp-384x32.bin` (four 8-row bands, solid
segments 32/64/96/128 px wide, i.e. 16/32/48/64 dots per half-row; 293 counts maximum at
48 dots, 315 at 64 dots, 4.2 V).

Further static findings (same session):

- Stock's temperature conversion `02052268` maps ADC <= 250 to 85 C (> 876 to 10 C), and
  the preflight `02052544` (case 0: read ADC1, convert, compare with 65) fails with
  error 9 above 65 C. A stock read of 0 would therefore also be refused. The SDK interlock
  is not stricter than stock, so stock's ADC1 must be non-zero on this unit.
- `020516c4` is a nine-band classifier of ADC1 (thresholds 249, 309, 390, 484, 577,
  681, 767, 876, 927), result stored at `020ac34e`. Its purpose is undecided.
- `02062fdc` is a background averager (ADC channel 1, N samples) that stores into the
  global `020b4bf8`; `02062fbc` and `02062f9c` set/get it. It does not touch any pad or
  analog register.
- None of the stock print entry points (`02054d20`, `02052944`, `0205265c`) writes a
  PM-chip, analog (0x88xx) or any other register beyond GPIO, ADC (0x9170..) and timer 3.
  Whatever makes ADC1 non-zero in stock is therefore set up at boot, not by printing.
- Stock's battery read `020033c0` configures PA6 (group 0, mask 0x40) before sampling
  channels 9 and 11. The earlier PA6 trial only floated it as an input; stock's exact
  arguments (input, pull flag 1, level flag 1) have not been replayed.
- The ledger tool does not follow task-table or function-pointer calls, so it cannot
  yet list the boot-time setup; the boot path has to be found through the tables set up
  at `02000000` (task ids 1..11).

Open differences between the SDK runs and the stock run that have never been varied:
the screen and rear camera were disconnected in every SDK test (stock ran with them
attached), and every SDK run started through the forced BootROM instead of a normal
stock boot.

## APIs and bench

Existing raster/feed/photo/cancel commands remain. APXD printer additions:

| Command | Request | Behavior |
| --- | --- | --- |
| 7 CHECK | Empty | Paper/temperature/timer sampling, no motion/heating |
| 8 SHUTDOWN | Empty | Disable power, motor and head |
| 9 TEMPERATURE LIMIT | u32 index0..65 | Can only lower the stock-derived limit; idle required |
| 10 RAW ADC PROBE | u32 channel0..11 | Sensor-only conversion and timer check; cannot start a job |

INFO adds sample validity, selected ADC channel/raw result, measured timer rate,
temperature limit, heat-pulse count/largest count, cancellation count and
software head/motor/power state. GPIO readback is distinct from electrical
current measurement; zero software pulse/step counts are not a print result.

Build `BOARD=silvergear-3222882 EXAMPLE=printer-test APX_SPI_TRACE=0
APX_LAUNCH_TRACE_MODE=0`. From the workspace root, the host bench is:

```sh
PYTHONPATH=apxtool/src python3 apxtool/research/silvergear_printer_test.py \
  --manifest appotech-sdk/build/silvergear-3222882/printer-test.manifest.json \
  --stock stock-backups/silvergear-3222882-20261102/stock.bin \
  --output appotech-sdk/research/silvergear/printer-test-new --timeout 300
```

The bench first requires the exact APXD build/board and runs CHECK, then accepts explicit
JSON commands on stdin (`check`, `adc`, `feed`, `raster`, `limit`, `cancel`,
`shutdown`, `info`, `recover`, `reload`). It never automatically proceeds from sensing
to heating. Recovery acknowledges BootROM and clears/verifies owned RAM.
The CLI also exposes `printer check`, `shutdown`, and `temperature-limit`.

`EXAMPLE=printer-usb-power-test` is a separate RAM diagnostic. Once USB has
been configured, loss of the PM external-power indication cancels the current
job and, after two seconds, runs sensor-only CHECK and raw channels0..11 once.
It never automatically feeds or prints. Reattach the bench with
`--attach --observe` to preserve its result; use APXD LOG to retrieve the saved
per-channel battery-only observations. No camera/display driver is initialized.

`printer-display-test` compares LCD initialization; `printer-phy-test` compares
the stock PHY reset while unplugged and always reinitializes USB after CHECK;
`printer-adc-timing-test` compares the native ADC transaction timing. These are
isolated diagnostic examples, not additional production printer requirements.
All remain sensor-only unless a host explicitly submits an ordinary job;
the normal driver's thermal interlock still applies to those ordinary jobs.
The bench accepts `reload` with target `manifest` and new `output` paths,
validates them before shutting down the current SDK, and starts the new bench
immediately after verified recovery. Its normal initial CHECK remains gated
by the exact build/board handshake.

## Current validation

The real GPIO driver feeds a 384-bit physical-edge shift/latch model for the
production and native-reference backends, checking every strobe's raster parity,
blank control and cleanup. Printer/GPIO/ADC, battery/SPI, signal phases and offline
orchestration models pass ASan/UBSan. Hardware production printing, cancellation,
mid-print paper loss and lowered-limit rejection passed on November7. Three
independent cold BootROM launches each produced blank control followed by three
recognizable bars, and the corrected camera demo printed a recognizable real
image and resumed preview. The linked acceptance record includes exact runs,
button calibration and verified recovery.
Historical build IDs and pending steps above describe their original test stage.
