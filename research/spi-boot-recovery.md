# Recovering a camera that no longer cold-boots (forced BootROM)

> Flash layout, free space for applications and ways to get more: see [`spi-flash-map.md`](spi-flash-map.md).

State on 2026-10-16: the SDK SPI image `bc2086e3…` (build `860fcba4ab0e43a5`)
verified by full readback but the camera does not start on a real power cycle.
See `spi-boot/boot-attempt.json`. Restore image:
`spi-boot/original.bin` (SHA-256 `063e10e7…`, bootstrap/app identical to
`flash.bin.golden`; they differ only in `0x1ba000..0x1befff`, the photo area).

## Why the rescue must be fast

The forced BootROM (`1908:3319`, `BLDR v1.00`) reloads its watchdog (SPR
`0x880c`, `0x5001d`) only from the USB interrupt handler (BootROM
`0x104878..0x104944` calling `0x102eac`). With no host traffic it resets about
two seconds later (kernel log: enumerate, ~1 s to the `sg` node, disconnect at
2-3 s, repeat). `boot/flasher.c` (formerly `rom-rescue.c`) therefore writes `0x5500` to `0x880c` on
every entry (reads back 0, `watchdog-power.md`; a request can set `FLAG_KEEP_WDT` to leave it armed, as for a stock camera's loader) and the host never idles.

## SPI bring-up

The BootROM SPI driver (`0x104ac4..0x104d20`) uses the same register protocol as
the payload: CS is bit `0x4000` of `0x9000`, receive mode is bit `0x80`, bytes go
through `0x9250`/`0x9244` and `0x9248` bit 0. The payload first reads the JEDEC ID
with the state the ROM left (`spi_path: rom-state`) and only then applies the
SDK setup (`sdk-init`). The short on the SPI lines must be released before this.

## Procedure

Run `apxtool` from its repository checkout rather than an older installed copy. From the directory that contains both
repositories (`appotech-sdk/` and `apxtool/`):

```sh
make -C appotech-sdk/boot
cd apxtool && export PYTHONPATH=$PWD/src      # apxtool repository, next to this one
python3 -m apxtool rom-rescue jedec            # read-only; start this, then:
#   unplug USB, hold the short, plug USB, release the short once BLDR appears
python3 -m apxtool rom-rescue dump bricked-dump.bin
python3 -m apxtool rom-rescue --yes restore YOUR-STOCK-BACKUP.bin --backup bricked-backup.bin
```

`jedec` prints the JEDEC ID, SPR snapshot (`0x880c` must be 0), the boot-header
validity and the header's load fields. `restore` re-reads the chip, records its
hash, writes only changed sectors with per-sector verify, commits sector 0
last, and ends with a full readback; the result must be `063e10e7…`. If the
device drops mid-way the tool waits for the next ROM cycle and resumes (backups
get `.retryN` names). Every step needs a fresh `sg` node, so no manual re-entry.
Plan B is an in-circuit `flashrom -p serprog` write of the same image.

## Why the first SPI image died, and how the next one reports it

Recovery was verified on hardware on 2026-10-16 (`readback_sha256` = `063e10e7…`).
What the BootROM and vendor bootstrap do on a cold start (from disassembly):

1. BootROM accepts the header (`BLDR`, `0x55aa`, flag `0x05`) and calls the header
   hook at `+0x4c`, which applies a PLL table and runs the bootstrap's DDR
   calibration (`SRAM 0x1238`, destructive pattern test over `0x02000000..0x023fffff`).
2. Only after that does the ROM load sectors 17.. to the header load address.
   The calibration therefore cannot overwrite the SDK image.
3. Bootstrap main (`SRAM 0x300`): save the ROM stack page to `0x1e6c`, run `0x760`
   (UART route PA7 and clock-gate setup), call the application entry.
   The same bootstrap prints diagnostics to the UART (115200 8N1, TX pad PA7).

That leaves the SDK's own start-up as the suspect. It had several silent exits
(`apx_watchdog_reset()` in `apx_spi_boot_prepare` and `apx_runtime_init`) that
fire when the incoming PLL/timer registers or the saved stack page differ from
what a RAM launch from the stock firmware shows. The rehearsal could not exercise
that state. A reset loop looks exactly like a dead camera.

Changes:

* Early failures now record the cause and fall into the BootROM USB loader
  instead of resetting (`apx_spi_boot_fail`). The camera then shows up as
  `1908:3319` and can be recovered or inspected without shorting anything.
* `APX_SPI_TRACE=1` builds add a flight recorder (`include/appotech/trace.h`):
  breadcrumbs and register snapshots programmed into the unused last sector
  `0x1ff000` and echoed on the UART. It disables the SDK watchdog on cold boot so
  a stall stays a stall. Decode with `apxtool trace FILE` (sector or flash dump)
  or `apxtool rom-rescue trace`. The model test is `tools/test_trace_model.py`.

```sh
make EXAMPLE=camera-demo BUILD=build/spi-boot-trace APX_SPI_TRACE=1
python3 tools/spi_image.py --original research/spi-boot/original.bin \
  --manifest build/spi-boot-trace/camera-demo.manifest.json \
  --output build/spi-boot-trace/trace.spi.bin
```

Install it like any SPI image (README, "SPI boot"), cold-boot, then read the sector:
from the running SDK `apxtool debug --port P flash read t.bin --address 0x1ff000
--length 4096`, or after a forced-ROM entry `apxtool rom-rescue trace`. A restore
of `original.bin` erases the sector again.

## Outcome (2026-10-16): SPI cold boot works

Production image `5041b5a00ee4f4d9` boots from SPI with USB, display, battery indicator,
Power-button start and USB-plug start all working. Recovery from any state needs no programmer.
Everything below was found on hardware; each item is a cold-boot-only gap that a USB launch
hides because the stock firmware had already run.

| Symptom | Cause | Fix |
| --- | --- | --- |
| Reset loop right after `T02` | The BootROM leaves its watchdog armed (selector 5, ~1.8 s) and the SDK zeroed 1.27 MiB byte-wise before feeding it | `clear()` uses word stores and feeds every 64 KiB; feed at entry |
| No USB enumeration | `apx_usb_init` skipped the BootROM's `0x9840 \|= 1`, then `\|= 4` | added, with settle delays |
| Clock bring-up slow, watchdog reset | The SPI path dropped PLL1 144 -> 96 MHz only so `apx_clock_init` could raise it again | no transition when the bootstrap already left 144 MHz |
| Silent resets | `apx_watchdog_reset()` on unexpected state | record the cause, fall into the BootROM loader |
| Test pattern missing without a sensor | `apx_camera_files_init` failed on NACK | colour bars, error still reported |
| Camera shuts off after Power boot / does not start from USB | The always-powered PM chip is not initialised on a cold boot (the SDK's own power-off even leaves `0x40=0x02, 0x44=0x10`) | `apx_power_domain_enable()` replays stock `02026624(1)` + `02026818(1)` first thing |
| Battery ADC channel 9 reads 0 | PM register `0x54` bit 5 (battery divider) not set on USB-plug boots | set in the same routine |
| Battery reference (ch 11) reads ~620, not ~246 | SoC register `0x8868` bit 15 enables the internal reference; stock sets it, the SPI path never did | battery driver sets/restores it |

PM-chip lessons:

* PM registers live in the always-powered domain. They survive SDK reboots while USB or the
  battery stays connected, but fall back to defaults after a full power loss. A bad write
  persists until power is truly removed or `rom-rescue pm --apply` repairs it.
* The stock result (a running camera) is `0x40=0x16 0x42=0x50 0x44=0x00 0x54=0x21 0x58=0x0c 0x5a=0x08`.
  Register `0x5a` must stay `0x08`: clearing it cut USB power and the Power-button keep-alive.
* Writes go to the odd address (`reg + 1`); reads use the even address.
* Stock code is easy to misread on this CPU: the instruction after a call executes **after**
  the return (links point at PC+4), so `r2` set in a "delay slot" feeds the *next* call.

### Tools that made this tractable

* `apxtool rom-rescue {jedec,dump,trace,pm,restore}`: works with the hardware entry sequence,
  disables the ROM watchdog first, prints the flight recorder before overwriting it, and
  `pm --apply` repairs the PM chip without touching flash.
* `APX_SPI_TRACE=1` flight recorder (`include/appotech/trace.h`, `apxtool trace`), with
  `APX_TRACE_KEEP_WDT=1` to keep the watchdog running. It also logs raw battery samples and an
  ADC probe after each boot stage.
* `tools/dump_sprs.py --port P out.json`: 249 registers in 0.6 s. Diff a stock-launched run
  against an SPI boot (`sdk launch` from stock firmware gives the reference) and flip the
  differing registers one at a time in the good state: this found the `0x8868` bit.

### `debug bootrom` (fixed 2026-10-16)

`apxtool debug bootrom` and holding SHUTTER at power-on both hand control to the ROM loader
(`0x10385c`). From an SPI boot this used to hang (no USB at all). The stock `0xda` handler
(`0203c1cc`) clears SPR `0x18` bits 1, 2 **and 14** just before it returns to the ROM; the SDK
cleared only bit 1, and the vendor bootstrap leaves bit 14 set after an SPI boot (a RAM launch
inherits it clear). `apx_bootrom()` and `apx_spi_boot_fail()` now clear `0x4006` as the stock does.
Result: `BootROM ready`, `1908:3319 BLDR`, RAM access verified, and the loader falls back to the
SPI image by itself after its 2-3 s idle reset.

### `sdk launch` from the loader after an SPI boot (fixed 2026-10-16)

Launching a RAM image from the loader that `debug bootrom` hands you used to crash back to the SPI
image or hang with no USB. Three independent causes, found with the flight recorder, an SRAM crumb ring
and a live UART capture (`tools/uart_capture.py`):

1. **The CPU caches were off in every SPI boot.** The stock application enables both at start
   (`02022c10`: data cache window at `0x1808 = 0x02000000`, bit 0 of `0x1800`; `02022c54`: bit 0 of
   `0x2000`, base `0x2008 = 0`); a USB launch from the stock firmware inherits that, an SPI boot never
   got it. Symptoms: the SDK ran at roughly 5% speed (clearing the 1.27 MiB buffers took 0.95 s, the
   clock measurement loop 1.0 s, whose wrapped 14-bit timer hid it as "46 ms"), and the PLL routine,
   which depends on running from one instruction-cache line while memory is unavailable, crashed.
   `apx_runtime_init()` now enables a disabled cache first thing (`apx_caches_enable()`), and flushes the
   buffers the boot code zeroes. A boot to enumeration went from about 3 s to 0.9 s, and `T0f` (the loop
   at the SDK clock) now reads the stock-launch value, 1593 ticks.
2. **The handoff left stale cache lines.** `apx_jump_to_rom()` writes back and drops all of DDR from
   the data cache, then switches both caches off before entering the ROM loader. Whatever the loader
   launches enables its own caches from a clean state, as the stock start-up does. (Leaving them on made
   the new image's code and the old image's lines disagree.)
3. **USB driver bug**: a SETUPEND between `SET_ADDRESS` and its status stage cleared `pending_address`
   but not `address_wait`, so the next status-stage interrupt wrote `0xff` to `FADDR`. The host then
   reported `error -71` / "device not accepting address". Hit on roughly one launch in ten because the
   host resets and retries quickly while enumerating. Regression test: `tools/test_usb_model.py`.

Test harness: `tools/launch_trials.sh MANIFEST N` repeats handoff, launch and return without power
cycles and classifies each trial (OK, crashed back to the SPI image, hung with no USB). Results with
all three fixes are recorded at the end of this section.

Diagnostic toolbox added for this work (trace builds, `APX_SPI_TRACE=1`):

* `APX_LAUNCH_TRACE_MODE`: what a USB-launched run records (0 none, 1 flash log in sector `0x1fe000`,
  2 UART only, 3 both, 4 SRAM ring). A crash that resets the chip loses RAM, but on-chip SRAM at
  `0x2800` survives; the next SPI boot imports the ring into its log (slots `0xc0..`, timers `0xa0..`)
  and echoes it on the UART as `Tcn`.
* Enumeration timeline crumbs (`T18` first poll, `T19` first host request, `T1d` configured) and, in
  UART-mode builds, one `U<n>:` line per USB event (bus resets, setup packets, address applied).
* `tools/uart_capture.py OUT SECONDS`: timestamped capture of the camera's UART on `/dev/ttyACM0`.
  Do not run two captures at once: the first holds the port and the second records nothing.
* Never put a slow loop in a trace build's boot path: a C replacement for the clock measurement loop
  was slow enough to trip the watchdog and bootloop the camera.

#### Fourth cause: PLL switch under a live USB link (found and fixed 2026-10-16)

After the three fixes above, about 9 of 100 launches still ended with the host reporting `device
descriptor read/64, error -71`, a *full-speed* first enumeration attempt, and finally `unable to
enumerate USB device`. The camera was alive (SRAM ring and watchdog showed the SDK looping); only its USB
transmissions were broken: SETUP and OUT worked, every IN data packet and even the SET_ADDRESS status
failed on the host.

* Found with `APX_LAUNCH_TRACE_MODE=4` (SRAM ring only, no UART/flash traffic that would change timing),
  `APX_USB_ENUM_TIMEOUT_TICKS` (diagnostic reset when the host never configures), `tools/launch_trials.sh`
  with `RING_DIR=...` (saves the ring of every trial) and `tools/decode_ring.py`. The ring now also holds
  EP0 events (`0x96` RxPktRdy, `0x97` data packet, `0x98` SENTSTALL/SETUPEND) and full 32-bit PHY/clock
  register snapshots (`0x9a`), before and after the first bus reset.
* The registers of failing and working boots were identical, and restarting the USB core (pull-up off,
  PHY restart) did not help, so it was not in the USB init sequence. Failing launches correlated 1:1 with
  the host's first attempt being full-speed (journalctl).
* A launch build with `CPU_MHZ=96` (no PLL1 switch) went 40/40; a cold SPI boot never switches either.
  The 96 -> 144 MHz switch in `apx_clock_init()` happened while the ROM loader's USB link was still
  live. `apx_clock_init()` now detaches the pull-up (POWER bit 6) first; `apx_usb_init()` reconnects after
  the clocks are final.
* Results: 144 MHz launch builds with the detach, 60/60 (trace ring build) and 60/60 (production
  configuration `e35c4361bc8e3b29`). Before: 2/12, 2/12, 5/40 (restart experiment), 2/40, 1/8.
* An enumeration-stall USB core restart was tried and removed; it did not rescue any failing boot.

### Other examples booted from SPI (2026-10-16)

Each image was built with `APX_SPI_TRACE=0`, packaged with `tools/spi_image.py`, installed over USB with
backup and `--expect-current-sha256` (readback matched), and booted by `debug bootrom` + loader idle reset.
Images that do not initialise SPI (gpio-demo) cannot run `flash install`; launch camera-demo and install from it.

| Example | Build | SPI-boot result |
| --- | --- | --- |
| gpio-demo | `393e374940ee4e19` | Buttons and display confirmed by the user on the camera. SD mounted (FAT32) on 5 of 5 SPI boots; 2 MB random round trip through the card identical three times. |
| jpeg-demo | `dc4e7ce3d56a3ae2` | Initial preview and hardware baseline decode 220x176 up to 2048x1536 without error; PNG/BMP/TGA/GIF/PPM and software JPEG paths decode; truncated and oversize inputs fail with 0x201/0x203; `encode-sample` writes a valid 320x240 JPEG to the card. |
| microphone-demo | `7306a503e654b810` | Captures at 8/16/32 kHz give the full sample count with no timeouts, a guard intact, a quiet noise floor around 20-30 RMS and clearly higher RMS while making sound; `microphone save` writes a valid WAV to the card. First ~100 ms of each capture has a start-up transient. |

SD note: with a card freshly inserted into the running camera every `CMD17` read was rejected for several
launches (controller data-status bits `0x200000` instead of `0x400000`, data itself correct); after that
the same builds mounted 12 of 12 times, in RAM launches and SPI boots, so it was a transient of the freshly
inserted 16 GB SDHC card, not a boot-path problem. Earlier SD validation used a 1.9 GB SDSC card; SDHC reads and
writes are now exercised too. If it recurs, build with a temporary bypass of the data-status check in
`apx_sd_read()` to tell a status quirk from corrupt data.

### LVGL from SPI (2026-10-17)

Two separate problems stood between the LVGL demo and an SPI boot.

1. **The image did not fit the SPI layout.** LVGL's code and assets (950 KB) live in the codec-RAM bank at
   `0x022e0000`, above the 1.4 MB window the BootROM loads, and a flat image up to there would be 2.4 MB. The
   packer (`apxtool/spi_boot.py`) now stores such segments as *overlays* in free flash after the main image and
   writes a CRC-checked table at flash `0x1fc000` (magic `APXO`, up to 15 entries of flash offset, address, length,
   CRC-32). The SDK copies them into place with `apx_spi_overlay_load()` (`src/spi_overlay.c`; no-op for USB
   launches and images without overlays): the LVGL demo calls it right after `apx_usb_init()`, USB and watchdog are
   serviced during the ~1 s copy, each segment is CRC-verified, then data cache written back and instruction cache
   restarted. Host models: `tools/test_spi_overlay_model.py` (real packer output), packer tests in
   `apxtool/tests/test_spi_boot.py`.
2. **A timing window in the timer control register.** The LVGL production image fell back to the BootROM loader
   in a loop (UART: `4MB pass PSRM:` forever). The SPI-boot normalisation writes SPR `0x9800 = 0x721` and the clock
   init requires it to read back; but the register is in the 32 kHz domain and only accepts that write about
   70 ms (18 attempts at 4 ms) after the SPI entry. Images that reach the check later (trace builds, whose flash
   writes take milliseconds; some demos) passed with the old single write, a faster production image did not. The
   write is now repeated and verified for up to ~250 ms (`apx_clock_prepare_spi()`, which also returns *why* it
   refuses: 1 PLL pair, 2 timer control, 3 period, 4 write never read back). Eight consecutive SPI boots of the
   production LVGL image all needed exactly 18 attempts and all came up. The window turned out to be tied to
   time since *reset*, not to SPI entry: the same image repacked at 50 KB (the BootROM loads it ~260 ms sooner)
   needed 84 attempts (~340 ms). The retry window is therefore 3 s, with the watchdog fed throughout.

Production builds now also keep **boot marks**: 31 (id, timer) pairs in SRAM at `0x2f00` (`src/boot_marks.c`), costing
a few stores and no flash or UART traffic. After a failed SPI boot the camera sits in the BootROM loader, where
`apxtool rom-rescue marks` prints them; on a running SDK use `debug read 0x2f00 256 --output F` and
`apxtool.trace.format_marks`. This is how the second problem was found without changing the timing that caused it.
Diagnostic builds can also record into the SRAM ring only, after importing the previous run's ring
(`APX_SPI_TRACE=1 APX_SPI_TRACE_RING_ONLY=1`).

#### After the LVGL fixes (2026-10-17)

* **Reset after a few DOWN presses** was an endless loop inside LVGL, not an exception: with a single display
  buffer `refr_configured_layer -> wait_for_flushing` runs `while(disp->flushing);` unless a wait callback is set,
  and the port only completed a flush from `apx_lvgl_port_poll()` outside `lv_timer_handler()`. A frame started
  while the display DMA was still running spun until the watchdog fired. Found with `LVGL_INSTRUMENT=1`
  (`-finstrument-functions` on LVGL; a 30-entry call trail in SRAM `0x3200`, previous run at `0x3300`) after the
  post-mortem record (`debug read 0x3000 128`: smallest stack/heap headroom, longest `lv_timer_handler`, last key)
  had ruled out heap, stack and slow renders. Fixed by `display_flush_wait()` in `src/lvgl_port.c`. This also
  showed that **CPU exceptions on this chip do not reach SDK code** (stubs at the standard vectors in SRAM never
  ran; an illegal instruction or an unmapped load just resets the camera), and that unaligned word loads do not
  trap. Use the post-mortem record and the call trail rather than expecting a fault handler.
* **The 4 KiB stack was too small**: LVGL widgets need about 5.1 KB, so the stack is now 16 KiB
  (`0x02200000..0x02204000`, also in the manifest reservations, `sdk.py`, `debug.c`, `hw.py`).
* **printf/stderr reach UART0 TX (PA7, 115200 8N1)** in the LVGL demo (`apx_uart_console_enable(1)`); otherwise
  they only go to the 4 KiB USB log ring (`debug logs`).
* **Flash space**: the LVGL image used ~1.6 MB because a 9 KB peripheral-driver segment at `0x02220000` forced the
  packer to store ~620 KB of zeros between it and the main code. The LVGL link now keeps those drivers next to the main code
  (`build/ram-compact.ld`, generated from `linker/ram.ld`), so the BootROM load block is 50 KB and the image ends
  at flash `0xfa000`; `[apx_spi_image_end(), 0x1fc000)` (about 1 MB) is free for application data. Other
  examples (camera, JPEG, audio, microphone) still carry the gap below their large extra-RAM code segment.

### CPU exception handler (2026-10-17)

`src/exception.c` / `include/appotech/exception.h`, installed by `apx_runtime_init()` on every boot.

* **Vector layout.** The vendor bootstrap's exception table is in SRAM at address 0 with **16-byte slots**
  (exception number times `0x10`, not the usual `0x100`): reset `0x00/0x10`, bus error `0x20`, data/instruction
  page fault `0x30/0x40`, tick `0x50`, alignment `0x60`, illegal instruction `0x70`, external interrupt `0x80`,
  DTLB/ITLB miss `0x90/0xa0`, range `0xb0`, syscall `0xc0`, FP `0xd0`, trap `0xe0`. The bootstrap's handler
  (SRAM `0x1c2c`) only arms the watchdog, so every fault ended as an unexplained reset. An earlier probe that
  assumed 256-byte slots never ran for that reason.
* **What the SDK does now.** The slots `0x20 0x30 0x40 0x60 0x70 0x90 0xa0 0xb0 0xc0 0xd0 0xe0` are rewritten
  (`l.ori r3,r0,vector; l.movhi/l.ori r4,handler; l.jr r4`); tick, external interrupt and reset stay with the
  vendor code. `apx_exception_common` (assembly, no stack use) stores the vector, SPR `0x20/0x30/0x40`, r1/r2/r9 in a
  record at SRAM `0x3400`; `apx_exception_report()` on a fresh stack adds the top 8 stack words, prints one line on
  UART0 (`EXC 00000070 illegal instruction S20=.. S30=.. S40=.. SP=.. LR=..`), then does the fast watchdog reset. A
  second fault while reporting resets at once. The next boot moves the record aside: `debug info` shows
  `previous_exception` and the boot log gets `previous run died in CPU exception ...`; it is reported for exactly
  one boot. The original vendor slots are put back before a BootROM handoff (`apx_bootrom`, `apx_spi_boot_fail`).
* **Verified on hardware** with `examples/exc-probe` (test 9 jumps to the illegal-instruction slot: record, UART
  line, reset and next-boot report all work). Host model: `tools/test_exception_model.py` (slot encodings, restore,
  one-boot lifetime, stale busy flag - a bug that the test found and a mutation check confirmed).
* **Limits found on this chip.** `l.sys`, `l.trap`, custom or reserved opcodes and loads from unmapped addresses
  (bus read at `0x7ffffff0`, SPR reads outside the implemented set, `0xf0000000`) do **not** reach the vector table:
  the CPU stalls or resets and the watchdog ends it, so no handler can report those. Unaligned word loads do not trap.
  A call through a NULL pointer lands in the vendor reset stub and was recorded as vector `0x30`, with SPR `0x40`
  holding a code address; the SPR roles (standard EPCR0/EEAR0/ESR0 would be `0x20/0x30/0x40`) are therefore
  not asserted and the fields are named by SPR number. For hangs and stalls use the post-mortem record
  (`debug read 0x3000`), boot marks (`rom-rescue marks`) and the call trail (`LVGL_INSTRUMENT=1`).
* **Deliberate resets raise vector `0x30`.** A software fast reset (`apx_watchdog_reset()`, i.e. `debug system reboot`,
  the handler's own reset, fatal init errors) makes the chip take vector `0x30` while the CPU spins, with SPR `0x40`
  inside that spin loop. The SDK flags such resets (`apx_exception_expect_reset()`, SRAM `0x3470`) and the handler
  then just waits for the reset to finish; otherwise every graceful reboot showed up as a "crash". A hang that lets
  the watchdog *expire* does not raise any exception (tested with `exc-probe` test 10), so hangs stay invisible to
  the handler. camera-demo (build `d6459a452c5aaa70`, image `cf4952a3...`) is installed with all of this.
