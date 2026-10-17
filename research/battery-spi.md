# Battery indication and independent SPI NOR

The camera demo now samples battery voltage alongside the button ladder and
shows a five-segment battery icon over live preview. A `+` denotes external
power present, **not a verified charging/full signal**. The levels are coarse
voltage bands, not a percentage or a fuel-gauge estimate. Charging controls and
automatic low-battery shutdown are unchanged.

## Evidence and conversion

Stock main image mapping: SPI offset `0x2200` → `0x02000000`.
Raw JT5295 little-endian/no-delay instructions take precedence over generic
OpenRISC decompilation. `AnnotateBatterySPI.java` adds the verified labels and
comments to Ghidra's `flash.bin.golden`; see `battery-spi-ghidra.txt`.

`02002d48` samples ADC channels 9 and 11 and averages sixteen pairs. Its
`0202e794` wrapper calls on-chip SRAM `00000804`, which loads the calibration
word at `00001fec`, multiplies channel9 by it, divides by channel11 and doubles
the quotient. This is not a BootROM USB or battery API. Saved low SRAM and its
hashes are in `battery-stock-evidence.json`; the current calibration is 803.
The SDK implements the arithmetic itself:

```
mean9 = sum(channel9) >> 4
mean11 = sum(channel11) >> 4
millivolts = 2 * floor(mean9 * calibration / mean11) + 20
```

The stock threshold table at `0205bd40` contains 3660, 3762, 3860, 3964,
4060 mV and a zero sentinel. Stock compares with strict `>` and uses 30 mV
hysteresis. The final zero sentinel makes its highest raw state 6; the SDK
normalizes this to five filled segments (public levels 0..5). It does not
add a percent field. Readings with zero/clipped reference, clipped battery,
voltage outside 2500..5000mV, missing calibration or ADC timeout are invalid;
a previously valid reading expires after three seconds without an update.

ADC `9170` selects/enables/starts conversion, bit6 is busy; `9178` is 10-bit
result, write-only divider `9174=71` matches stock. `adc.c` arbitrates one
conversion between channel3 buttons and the battery pair; clients cannot
change the channel before the previous owner collects its sample. Reference
counting restores inherited ADC control and the owned `8840` clock-gate bit
only after both clients shut down. Battery polling also runs during cooperative
hardware maintenance. No ADC interrupts are enabled.

`020267d0` returns `88e0` bit21 and stock uses it as supply-present. The driver
publishes this as `external_power` plus the raw PM control word. Bit20 and
charger-complete/charge-active semantics have not been established. The SDK
never changes the PM charging settings to obtain battery status.

## SPI NOR controller

The existing loader `apxtool/firmware/spi.c` (the former vendor-callback flasher, since replaced by `boot/flasher.c`) calls vendor functions. The SDK
ports the register sequences from `02025750..02025bc8` and flash commands from
`02032cfc..02033070`; it calls no vendor SPI routines. Raw controller evidence
is in `spi-controller-vendor.disasm` and `spi-main.disasm`.

| Registers | Verified behavior |
| --- | --- |
| `8850[31:28]=3`, `8844 bit3=0`, `8840 bit20=0` | Stock clock/reset selection |
| `8344[21:20]=1`, GPIO PA8/10/11 | Stock flash pin routing; preserve other fields |
| `9000=01115005` | Controller setup; bit14 is CS high, bit7 byte receive, bit1 DMA |
| `9244=1`, `9248 bit0` | Byte start/completion, data `9250` |
| `924c=5` | Stock divider (36MHz/6MHz−1 in stock arithmetic); reused at both SDK CPU profiles |
| `900c=85` during receive DMA | Stock DMA receive mode |
| `9254=words−1`, `9258=address`, `925c=06000000` | DMA length, source/destination and controller parameter |
| `9244=20000`, then `2`; `9248 bit1` | DMA event clear/start/completion |

`9f` reads JEDEC `5e4015`, a 2MiB board device; initialization rejects any
other ID instead of guessing its geometry. `03` reads a 24-bit byte address;
`05` reads status. Reads accept arbitrary byte offsets and lengths. A guarded,
32-byte aligned 4KiB bounce buffer isolates caller cache lines. DMA handles
whole 32-byte blocks and bounded PIO handles the tail. Cache maintenance runs
before/after DMA. Chip select is released on every error; DMA addresses and
configuration are cleared before BootROM/power handoff. Pin/clock fields are
restored without overwriting other peripherals' fields.

`apx_spi_program` and `apx_spi_erase_sector` implement `06` write-enable, `02`
page program and `20` 4KiB erase, poll WIP, and require WEL. Programming rejects
page-crossing writes and erase rejects unaligned addresses. These C APIs are
model-tested only; **no physical SPI erase/program has been issued for this
milestone**. APXD intentionally exposes information and reads only.

## Interfaces and checks

- `appotech/adc.h`: cooperative shared ADC ownership.
- `appotech/battery.h`: init/poll/status/shutdown; valid millivolts, coarse level,
  raw channels/calibration, supply-present, error counters.
- `appotech/spi.h`: init/status/read/program/erase-sector/shutdown.
- APXD operation12 SPI: command0 INFO empty → JSON; command1 READ has two LE32
  words (address, byte count ≤4096) → exact bytes. Other commands are rejected.
- APXD operation13 battery: command0 INFO empty → JSON. Header stays APXD v1.
- Host `BatteryClient`, `sdk_flash.FlashClient`; existing `apxtool.flash` and
  SCSI workflows remain available.

Run `tools/test_battery_spi_model.py`, `tools/test_gpio_model.py` and
`tools/test_system_model.py` for arbitration, hysteresis, failure cases,
wraparound, DMA boundaries, NOR semantics, malformed frames and shutdown.
The model programs/erases a simulated NOR only. `tools/test_battery_spi_live.py`
compares a complete 2MiB readback against the saved original, exercises boundary
lengths and malformed requests, and runs camera/SD/speaker/microphone/battery
coexistence. Its results are in `battery-spi-live/acceptance.json`. ELF direct
call/inert-slot verification is in `battery-spi-elf-audit.json`. Recovery records
are separate. The user confirmed the displayed battery indicator looks right.

## Accepted hardware build

The tested image is `build/battery-spi/camera-demo.manifest.json`, build
`bbcfdd5d939913ed`. Aggregate acceptance records
155 passing host tests (plus six subtests), native models and ELF checks, 60.37s
coexistence at 12.42 preview frames/s, 109 duplex audio jobs, 75 battery updates,
and three successful same-image BootROM recoveries. Full 2MiB readbacks before
and after recovery match SHA256
`063e10e7df19095f5ee8217cb031e03297941194f460959a47056c12a38dab66`.
The primary reservation retains 12,624 bytes free, EXTRA_RAM 14,656 bytes free,
and the configured heap remains 16KiB with a separate 4KiB stack.

Concurrent LVGL linker additions were preserved. The subsequently rebuilt
camera image `build/battery-spi-current/` has identical load addresses, memory
sizes, flags and bytes after substituting only its build-ID string; see
`battery-spi-current-equivalence.json`. The accepted hardware artifact was kept
unchanged. The 96MHz fallback was built/audited without replacing the live
144MHz image. Neither fallback hardware operation nor physical flash writes
is newly claimed by these results.

## Cold SPI boot requirements (2026-10-16)

The numbers above were measured on USB launches, where the stock firmware had already
prepared the board. On an SPI cold boot two things must be done by the SDK:

1. PM chip register `0x54` bit 5 set (`0x21`): powers the battery divider; without it channel 9 reads 0.
2. SoC register `0x8868` bit 15 set: enables the internal reference sampled by channel 11.
   Without it channel 11 reads ~620 instead of ~246 and `millivolts` is invalid.

`apx_power_domain_enable()` (src/power.c) does the first as part of the stock power
sequence; `apx_battery_init()` does the second and `apx_battery_shutdown()` restores it.
The driver also discards the first conversion after a channel change and waits ~33 ms before
sampling. Verified: 4250 mV, level 5, `adc_battery` 648, `adc_reference` 246.
