# Silvergear SPI install: offline analysis and open items (2026-11-08)

Goal: boot an SDK image from the camera's SPI flash so later work needs neither the forced-BootROM short nor a
watcher script. **Nothing has been written to the camera; the packer exists and is host-tested only.**

## What the BootROM and bootstrap do (disassembly)
- The BootROM is byte-identical on both cameras (sha256 `f5b68ff4...`, see `power-ddr.md`). Header check
  (`100548..`): `hdr[2]==2`, `BLDR`, `0xaa55` trailer, flag byte `+10` (bit 2 set: no 512-byte checksum). It reads only
  the record at `+0x20` (load address, start sector, sector count). Header `+0x18/+0x1c` (stock sizes) are not read.
- Header hook at `+0x4c` runs the bootstrap's DDR calibration (Silvergear: SRAM `0x10fc`, Action: `0x1238`;
  destructive pattern test over `0x02000000..0x023fffff`) **before** the ROM loads sectors 17.. to the header load
  address, so it cannot overwrite the SDK image.
- Silvergear bootstrap main: `0x2fc` save marker, `0x358` copy the ROM stack page, `0x6c4` UART/GPIO setup (divider
  1249 = `APX_UART_BOOT_DIVIDER`), then `0x340/0x344` `l.movhi r2,0x200; l.ori r2,r2,0x5ac; l.jalr r2` = stock startup
  `0x020005ac`. The Action bootstrap has the same hook at `0x328`; the packer patches the pair per board.
- Bootstrap bytes `0x200..0x21ff` differ from the Action camera's (6503 of 8192), so the packer pins the exact
  Silvergear hash and the exact stock hook instructions and refuses anything else.

## What exists
- `apxtool/src/apxtool/spi_boot.py`: `build_image(original, manifest, board='silvergear')`; `tools/spi_image.py --board silvergear`.
  Changes only header `0x20/0x28` and the hook at `0x540`. Overlays refused (the Action table sector `0x1fc000` is stock
  data here). Tests: `apxtool/tests/test_spi_boot.py` (synthetic and against the real `stock.bin`).
- Dry run (no camera): stock.bin + `printer-camera-demo` build `ab2437414b9aa886` gives a 4 MiB image where 289 of 1024
  sectors differ (flash `0x0..0x123000`). The stock app and its resource index (`0xb0e00`) are overwritten; stock data beyond
  the loaded block is preserved. Restoring stock means writing `stock.bin` back.

## Open items before any install
1. **Recovery path: done offline, not yet exercised on the camera.** `boot/flasher.c` has a `SILVERGEAR_FULL_RESTORE` build
   (`make -C boot silvergear-full-restore`, payload sha256 `2e4e4ba0ad02bfe9b0026bd6895791e9c312d553d85731ff5789eed56d543e51`,
   SRAM and DDR links byte-identical): 944016 chip only, SRAM window only, request flags `0x53464752` (the three-sector
   incident payload `40411b41...` keeps `0x53470000`; each refuses the other's flags), no PM writes, protection bits refused.
   `apxtool/src/apxtool/silvergear_install.py` makes the chip equal a 4 MiB target: validates the target (exact SHA-256
   given by the operator, reviewed Silvergear bootstrap except the three packer-rewritten fields, BLDR header, load record),
   reads and saves the whole chip first (never overwrites a backup), writes only differing sectors with the boot header
   sector last, verifies each sector and the full chip, journals every step. The same code restores `stock.bin` and installs a
   packed SDK image. Default is an offline preview; `--yes` needs `--expect-payload-sha256` and `--output`.
   Tests: `apxtool/tests/test_silvergear_install.py` (real payload logic through the loader model, mutation-checked).
   Still to do on hardware before an install: a read-only forced-BootROM dump compared with `stock.bin` (proves the read
   path and that the chip still matches), then a restore rehearsal only if you approve a write.
2. **SDK SPI driver: done, read path verified on hardware (2026-11-08).** `board.h` now carries `APX_SPI_JEDEC` and
   `APX_SPI_FLASH_BYTES` (Action `5e4015`/2 MiB, Silvergear `944016`/4 MiB); `spi.c`, the APXD status JSON and
   `apxtool/sdk_flash.py` follow the board. `tests/spi_model.c` runs on both boards (`tools/test_spi_model.py`,
   `tools/test_silvergear_models.py`). On the camera (RAM launch of `printer-camera-demo` build `c40efb13...`, read only):
   `debug flash info` reported JEDEC `944016`, 4 MiB, status 0; a full 4 MiB read took 12 s with 1024 DMA transfers and no
   timeouts; two reads were identical. **Erase/program through the SDK driver are not yet exercised on this chip** (the
   BootROM payload has written this chip before; the SDK driver uses the same register recipe).
   The dump differs from `stock.bin` in 12 sectors only (`0x381000..0x386000`, `0x392000..0x394000`, `0x3a1000..0x3a3000`):
   three JPEGs (about 14 KB, 9 KB, 9 KB) plus table/counter bytes, i.e. photos saved by the stock firmware after the backup.
   Saved as `stock-backups/silvergear-3222882-20261108/flash-read-sdk-20261108.bin` (sha256 `84486ff2...fce19`). Pack from this
   file, not from `stock.bin`, to keep those photos; restoring `stock.bin` would erase them.
3. **Cold boot: two board-specific defects found and fixed offline; the cold boot itself is still untested.**
   - `apx_spi_boot_prepare` restored the bootstrap's saved ROM stack page from the Action address SRAM `0x1e6c`. The
     Silvergear bootstrap saves it at `0x1c94` (same routine, data 0x1d8 lower), so a Silvergear cold boot would have taken the
     silent watchdog-reset path. Now `APX_SPI_BOOT_SAVED_STACK` in each `board.h`; `tools/test_spi_boot_layout.py` reads the
     address out of both stock bootstraps and fails if a constant drifts.
   - The flight recorder erased the Action sectors `0x1fe000/0x1ff000`, which hold stock data on this chip, and its chip check
     hard-coded `5e4015` (a Silvergear trace build would have recorded nothing). Now `APX_SPI_TRACE_SECTOR` /
     `APX_SPI_TRACE_LAUNCH_SECTOR` (Silvergear `0x3fe000` / `0x3fd000`, both erased) and the board JEDEC;
     `apxtool trace` and `rom-rescue trace` pick the sector from the dump/chip size; `tests/trace_model.c` runs on both boards.
   - Already board-correct: the PM keep-alive (`apx_power_domain_enable`, Silvergear branch) runs first in `apx_runtime_init` on
     an SPI boot; battery calibration address `0x1e14`.
   - The "hold SHUTTER at power-on" recovery gesture cannot work on this board (POWER is on the same ADC ladder). Recovery from
     a running image is `apxtool debug bootrom` or a 2 s Power hold in `printer-camera-demo`; early boot failures fall into the
     ROM loader (`apx_spi_boot_fail`); anything else needs the SPI short.
   - A trace image needs `HEAP_SIZE=8192` to fit the 128 KB runtime window (the Action trace build overflows it today too).

## Images prepared (scratch only, nothing written to the camera)
Base: `flash-read-sdk-20261108.bin`. Both change 290 sectors, flash `0x0..0x124000`; photos and the tail sectors untouched.
| Image | Build | image sha256 |
| --- | --- | --- |
| trace (`APX_SPI_TRACE=1 HEAP_SIZE=8192`) | `efab5481fa1cee47` | `eb8476012ade28ee108e97085f721e94b4af2a48741ae8e42ac7baea631a1039` |
| production | `e09366e4b9c74fd8` | `2884a65cccc7ecd35f12c51dcb2828c36827f4043a5f0e53aa37e04777c21421` |
Writer payload: `silvergear-full-restore.bin` sha256 `2e4e4ba0ad02bfe9b0026bd6895791e9c312d553d85731ff5789eed56d543e51`.

## Proposed hardware sequence (each write needs an explicit go-ahead on the exact hashes)
1. Install the trace image from forced BootROM with `python3 -m apxtool.silvergear_install IMAGE --expect-sha256 H --payload P
   --expect-payload-sha256 2e4e4ba0... --output BACKUPPREFIX --yes` (keep Power held; backup + journal are made first).
2. Cold boot it, watch UART (PA7, 115200), read the trace sector on failure (`rom-rescue trace`).
3. If it runs: install the production image over USB from the running SDK (`apxtool debug flash install`), then cold boot again.
4. Restore at any time from forced BootROM with the same tool and `flash-read-sdk-20261108.bin` (keeps photos) or `stock.bin`.

## Result log
- **2026-11-08 trace image installed (step 1), flash verified.** Forced-BootROM run of `silvergear_install` with the approved
  hashes (image `eb847601...`, payload `2e4e4ba0...`): backup `stock-backups/silvergear-3222882-20261108/install-trace-*.before.bin`
  (sha256 `84486ff2...`, identical to the earlier dump), journal `...journal.json` status `verified`, 290/290 sectors verified,
  boot header sector written last, full-chip readback sha256 = target. The first attempt that day failed before opening the
  loader (a fixed 8 s wait outlasted the loader's few-second timeout); nothing was written. The tool now opens the loader at
  once (`spi_init='none'`, payload keeps it alive) and retries only the read-only JEDEC probe (`--wait-flash`).
  The installed image has **not yet been cold-booted**.
- Restore (any time, forced BootROM, keep Power held):
  `python3 -m apxtool.silvergear_install stock-backups/silvergear-3222882-20261108/flash-read-sdk-20261108.bin --expect-sha256 84486ff2d2ba69b573b3ce4598b27206d37dae4eb8181d86569a9bb7707fce19 --payload stock-backups/silvergear-3222882-20261108/silvergear-full-restore.bin --expect-payload-sha256 2e4e4ba0ad02bfe9b0026bd6895791e9c312d553d85731ff5789eed56d543e51 --output stock-backups/silvergear-3222882-20261108/restore --yes --timeout 170 --wait-flash 90`
  (use `stock-backups/silvergear-3222882-20261102/stock.bin` and its sha `4874af74...` for a pure stock image, which drops the three photos).
- **2026-11-08 cold boot of the installed trace image: works (step 2).** From power-off on battery the owner saw the demo and
  USB enumerated. `debug info`: `boot_source: spi`, build `efab5481fa1cee47`, 144 MHz, USB high speed; `debug flash info`: `944016`,
  4 MiB, no timeouts; SDK log `SPI flash init 0`. Trace sector `0x3fe000` (`apxtool trace`): entry stack `0x3ffffc`, saved ROM stack
  page valid (`0x4f34`), PM `0x22 -> 0x36` with no failure bit and reg `0x5a` `0x09 -> 0x09`, timer control `0x721` before and after
  clock init, clock profile applied, recovery window passed, USB enumeration complete about 1.1 s after entry.
  Hardware proof of the saved-stack fix: SRAM `0x1c94` holds `0x4f34`; the Action address `0x1e6c` reads all zeros, so the unfixed
  code would have taken the silent watchdog-reset path on every cold boot.
  Not yet exercised: erase/program through the SDK driver (`debug flash install`), the production image (watchdog running), the
  Power-click LED toggle on the SPI-booted image, and the `apxtool debug bootrom` -> RAM launch round trip without the SPI short.
- **2026-11-08 production image installed over USB from the running SDK, no forced BootROM (step 3), flash verified.**
  `apxtool debug flash install install-production.spi.bin --backup usb-install-production.before.bin --expect-current-sha256 2089450b...`.
  A first attempt with `--expect-current-sha256 eb847601...` (the trace image) was refused by the tool's own check before anything was
  erased ("current SPI hash differs from expected; no erase performed"): the chip differed from that image in exactly one sector,
  `0x3fe000`, the trace image's flight-recorder log from its cold boot (two identical dumps; the log was saved as
  `trace-sector-3fe000-coldboot.bin`). The install then ran against the chip's real hash: 157 sectors, header last, journal
  `verified`, backup sha `2089450b...`, and an independent dump afterwards equals the production image
  (`2884a65cccc7ecd35f12c51dcb2828c36827f4043a5f0e53aa37e04777c21421`); photo sectors and `0x3ff000` unchanged. This is the first
  hardware exercise of the SDK erase/program path on the 944016 chip. Note: a trace build rewrites `0x3fe000` on every boot, so
  expect that sector to differ from any packed trace image after it has run. The production image has **not yet been cold-booted**.
- **2026-11-08 production image cold-boots (owner): demo and USB work. Defect reported: the 2 s Power hold did not power off.**
  Cause: `printer-camera-demo` mapped the hold to `apx_bootrom()` (a development hand-off, not a power-off), so the CPU returned
  to the ROM loader: display and peripherals off, PM chip still holding the main supply (blue power LED on), loader idle (on battery
  it never answers a host, so only the reset button or USB data recovers it). Fix in the demo: the hold arms power-off, the flash LED
  goes off, the overlay says "RELEASE POWER TO TURN OFF", and `apx_power_off()` runs when Power is released (stock waits for the key
  ladder to read "no key" before cutting the supply; with USB attached the supply stays up as in stock and the CPU idles). The
  BootROM hand-off remains `apxtool debug bootrom`. `apx_power_off()`'s PM tail (PM54 bit 5 clear, PM44=0x10, PM42=0xe0,
  PM40=0x02) matches the stock shutdown tail (`0200eb2c` -> `202b98c(0)`, `202b888(1)`, `202b950`, `2038800(0)`), but it has never been
  exercised on this board: the first test is the owner's.
  Chip state after the production boot: production image plus one crumb byte (`0x0d` at `0x3fe834`, `APX_TRACE_BOOTROM_CALL`
  logged by the still-running trace image when Power was held after the install). New image prepared (not installed): build
  `6c9483a28c98c72d`, sha256 `43a1bbc8741393e6b0771f5247c2fd8339b5fe4f96e39d47e6ea5b1cd14788c1`, 37 sectors (`0x2000..0x3ff000`,
  includes erasing the crumb sector), photos untouched.
- **2026-11-08 power-off image installed over USB (owner-approved), flash verified.** Image `install-poweroff.spi.bin`, build
  `6c9483a28c98c72d`, sha256 `43a1bbc8741393e6b0771f5247c2fd8339b5fe4f96e39d47e6ea5b1cd14788c1`; pre-write chip `ac14436e...`
  (re-read just before, equal to the approved state); 37 sectors verified, backup `usb-install-poweroff.before.bin`, journal
  alongside; independent dump equals the image; photos and `0x3ff000` untouched. Sector 0 was not rewritten (same load size as the
  production image). Host `apxtool debug system reboot` (graceful reboot) brought the camera straight back on the new build from
  SPI in about 2 s. The Power-hold power-off itself is still to be tested by the owner on battery.
- **2026-11-08 owner confirmation on the installed power-off image (build `6c9483a28c98c72d`): "both now work"** - the Power-click
  flash LED toggle and the 2 s Power hold power-off (battery, USB unplugged). First hardware run of `apx_power_off()` on this board: OK.
  **Current state of the camera:** SPI flash holds the SDK `printer-camera-demo` (image `43a1bbc8...`), cold boots from SPI, USB
  enumerates, `apxtool debug flash install` and `apxtool debug system reboot` work from the running image, so no forced BootROM or
  SPI short is needed for ordinary re-flashing. Stock firmware is not on the chip; restore from forced BootROM with the commands
  above (`flash-read-sdk-20261108.bin` keeps the photos, `stock.bin` is pure stock).
  Not exercised: `apxtool debug bootrom` followed by a RAM launch from that loader without the SPI short on this board, and the
  `apx_spi_boot_recovery` SHUTTER gesture (cannot work here, POWER shares the ADC ladder).
- **2026-11-08 `debug bootrom` -> RAM launch without the SPI short: works (owner not involved, camera on USB).** From the SPI-booted
  image: `apxtool debug --port P bootrom` printed `BootROM ready at /dev/sg0` (the loader only waits a few seconds, so launch at
  once); `apxtool --device /dev/sg0 sdk launch MANIFEST` (plain launch: DDR is alive, no `--stock-backup` stage needed) brought a RAM
  build up with `boot_source: usb`, 144 MHz, USB high speed, error 0. A full dump afterwards equalled the installed image (nothing
  written), and `apxtool debug system reboot` from the RAM-launched image returned to the SPI image (`boot_source: spi`, 6 s).
  Dev loop for Silvergear is therefore: build, `debug bootrom`, immediate `sdk launch`; `debug system reboot` (or a power-cycle)
  returns to the installed image; `debug flash install` when a build should become the installed one. The generic helper
  `Action_Digital_Kids_Camera_DOOM/tools/launch.sh MANIFEST` does the bootrom + launch step in one command.
- **Scope caveat (owner, 2026-11-08):** the RAM-launch / install loop above was exercised only with `printer-camera-demo`. The other
  SDK examples were written for the Action camera and have not been run on this board (hardware untested here, in particular
  the speaker and microphone, `audio-demo` / `microphone-demo`). Both compile for `silvergear-3222882`; `audio.c` drives the SoC analog
  block (`0x886c/0x8870/0x8874`, DAC and amplifier) with the PLL-compensated selectors in `board.h`, never checked against this
  camera's speaker/microphone wiring (the stock registry has no audio device, so any mute/amp-enable line is unidentified, like the
  flash LED was). The audio demos also preview an Action-sized JPEG from the SD card and do not use the Silvergear display geometry.
