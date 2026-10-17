# JT5295 GPIO, buttons and blue status LED

Evidence is the raw little-endian vendor instructions in
`gpio-vendor-raw.disasm`, inherited registers in `gpio-inherited.json`, the
physical press capture in `gpio-button-capture.jsonl`, and user observation on
2026-10-14. The main image mapping is file offset 0x2200 to CPU 0x02000000.
`AnnotateGPIO.java` labels the corresponding functions and threshold table in
Ghidra's `flash.bin.golden`; generic OpenRISC delay-slot decompilation does not
override the raw instructions. The SDK uses no vendor GPIO or ADC functions.

## GPIO registers

Stock bank indices 0..4 correspond to SDK A..E; offsets below add 4*bank.
The configuration/write helpers validate 16-bit masks. A generic read helper
also accepts bank 5, but no verified configuration/board mapping exists for it,
so the SDK deliberately exposes only banks 0..4. Package pin numbers and each
bank's physically bonded mask are unknown.

| SPR base | Verified stock behavior | Confidence / limits |
|---|---|---|
| 0x8000 | output latch write and input sample read | raw helpers 02023cb8/02023dc0; display bus and Power confirmed |
| 0x8040 | bit 1=input, 0=output | helper 02023ab8; existing LCD and SD bring-up |
| 0x8080 | pull-up mask | helper 02023b5c bit 0; stock SD pull configuration |
| 0x80c0 | pull-down mask | helper 02023b5c bit 1; electrical strength not measured |
| 0x8100 | GPIO wrapper clears selected bits | helper 02023e44; electrical role not established |
| 0x8140 | GPIO wrapper clears selected bits | helper 02023c28; electrical role not established |
| 0x81c0 | forced high mask | helper 02023ff4 bit 0; PB5 LED physically confirmed |
| 0x8200 | forced low mask | helper 02023ff4 bit 1; PB5 LED physically confirmed |
| 0x8380 | GPIO wrapper sets selected bits | helper 02023ed4; additional peripheral routing uses 8100 and mux SPRs |

`apx_gpio_configure` implements stock 0202ebc0 with drive disabled while the
latch/path/pulls change. An output's initial latch is written before output
enable. Override changes clear the opposite force first. All operations are
masked; no bank initialization clears unrelated pins. These RMW operations
require cooperative callers. GPIO interrupt delivery and generic alternate
function selection are not implemented.

The LCD reset/backlight/RS now use this GPIO library. Display bus routing and
SD routing retain their separately verified peripheral-specific setup.
Do not repurpose PA2: the SD driver must keep it driven low. Board buttons own
PB0/PB1, LED owns PB5, LCD reset owns PB6, and backlight owns PA6.

## Board inputs and LED

`ad-key` open 02002b14 configures PB0/PB1 as floating inputs. The scan function
0200326c first reads PB0: high selects stock code 31 (Power). Otherwise it
samples ADC channel 3 and compares against five records at 02090408, each
containing key code, nominal ADC, low bound, high bound as four u16 values.
Open expands each center by 29 and clamps the bounds to 0..1024. Idle ADC is
1023. PB1's relationship to the ADC pad has not been electrically traced.

| SDK button / bit | Stock code | ADC center | Captured pressed value |
|---|---:|---:|---:|
| SHUTTER / 0 | 26 | 0 | 0 |
| DOWN / 1 | 28 | 306 | 310 |
| UP / 2 | 27 | 484 | 493 |
| LEFT / 3 | 29 | 658 | 661 |
| RIGHT / 4 | 30 | 817 | 821 |
| POWER / 5 | 31 | digital PB0 high | PB0=1, idle ADC=1023 |

The user confirmed every button changed its screen tile to green and the blue
status LED blinked. The ADC path supports one ladder button at a time; ladder
chords are ambiguous and are not decoded. The digital Power input can coexist
with a ladder input. Power has no shutdown policy in the SDK.

`led` open 02002bd0 configures PB5 as an input and sets force-low. Its ioctl
02002ef0 sets 02023ff4 mode 1 for on, mode 2 for off. This is the blue status
LED, active high. The display backlight remains a separate display API. No
additional software-controlled board LED has been identified. Charging LED
and battery status are outside this milestone.

## ADC and sampling

Raw helpers 02027b10/7b78/7ba8/7bcc select ADC channel, enable clock gate 29
(active-low SPR 8840), write clock divider 9174, start through 9170 bit 7,
wait while 9170 bit 6 is set, and read low 10 bits of 9178. Stock requests
1 MHz: 144 MHz/(2*(divider+1)), divider 71. SPR 9174 reads zero on hardware
and cannot be used as a saved-divider snapshot.

The SDK inherits the vendor analog/reference setup under the RAM launch
contract. It implements asynchronous conversions, a 5 ms deadline, a 5 ms
sampling interval, and 20 ms stable-state debounce. Poll every <=5 ms.
An ADC timeout clears ladder state after debounce and increments a diagnostic
counter; it never reports a stale conversion as a fresh sample. USB and SD
remain cooperative, and SD wait loops also service button sampling.
There is no interrupt handler or vendor polling call.

The 16-event queue retains the newest events, drops the oldest on overflow,
and counts dropped events. Each event includes ticks, state, pressed, released.
Applications may use the state snapshot instead of consuming the event queue.
The initial capture's 40 dropped events predate host draining while the user
tested the controls; they are bounded queue overflow, not ADC failures.

Shutdown stops ADC conversion, restores the owned clock bit and PB0/PB1/PB5
configuration fields, and runs before the normal display/SD/USB BootROM
handoff. Divider 71 remains because its previous value is unreadable; stock
also uses 71. GPIO events, shutdown, ADC failure and wraparound are modeled by
`tests/gpio_model.c` with ASan/UBSan.

## Validation artifacts

`gpio-live-validation.json` records the initial named-button build's combined
SD/USB/GPIO read stress; `gpio-shared-api-live-validation.json` records the
display's transition to the shared GPIO API. `gpio-accepted-live-validation.json`,
`gpio-recovery-cycles.json`, `gpio-elf-audit.json`, `gpio-acceptance.json` and
`spi-after-gpio.bin` record final acceptance. SDK-owned RAM uses the existing
four reservations; GPIO adds no new reserved region. Card writes, card format,
and SPI erase/program are not performed.

Final build `255d52d8fda00a62` passed 126 host tests plus 6 subtests and all
four ASan/UBSan models. Sixty seconds of simultaneous GPIO/USB/SD work verified
17,479,680 bytes across 7,634 reads, with zero ADC timeouts or SD errors.
All five saved stock file hashes matched after the ten recovery cycles. Each
cycle completed the 70,400-pixel labeled display, checked both LED register
states, acknowledged BootROM return, verified loader INQUIRY/RAM access and
released every owned RAM reservation. The bottom 512 stack bytes remained zero.
The final ELF audit found 537 direct calls confined to SDK code and 2,690
inert slots. All four example builds retain their original reservations.

GPIO demo has 64,220 bytes of code and 4,917 bytes of normal read-only data.
Its primary runtime uses 103,160/131,072 bytes including the 16 KiB heap; BSS
is 17,568 bytes. Display workspace remains 63,360 bytes, FatFs codepage tables
483,036 bytes, and stack 4,096 bytes in the existing separate reservations.
The complete 2 MiB SPI hash remains
`063e10e7df19095f5ee8217cb031e03297941194f460959a47056c12a38dab66`, identical
to the pre-GPIO SD/display baseline. The final GPIO demo is running on port 7-1.
