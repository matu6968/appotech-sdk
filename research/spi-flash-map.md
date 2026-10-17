# JT5295 SPI flash map, free space and how to get more of it

Written for whoever builds the next application on this SDK and finds that the 2 MiB chip is tighter than it
looks. Everything here was measured on the camera (25Q16, JEDEC `5E4015`) unless marked otherwise.

Quick answer: `python3 tools/flash_map.py IMAGE_OR_DUMP.bin` prints the map below for any packed image
(`tools/spi_image.py` output) or chip dump (`apxtool debug flash read OUT.bin`), including how much is free.

## The chip

* 2 MiB, erase unit 4 KiB (sector), program unit 256 B (page, never across a page boundary), programming can
  only turn 1 bits into 0 bits, so **erase a sector before writing it**. Erase takes up to a few hundred ms.
* Not memory mapped: the CPU cannot execute or read it directly. Everything goes through the SPI driver
  (`apx_spi_read/program/erase_sector`; reads use DMA) or the BootROM at boot.
* The BootROM loads **one contiguous block** from flash to DDR `0x02180000` and starts the vendor SRAM bootstrap,
  which calls the SDK entry. That single block is what makes images big (see "Why images are bigger than the code").

## The map

| Flash range | Size | Contents |
| --- | --- | --- |
| `0x000000..0x0001ff` | 512 B | BootROM header: `BLDR` at +4, flags, load address `0x02180000` at +0x20, start sector 17 at +0x24, **load sector count** at +0x28 (patched by `tools/spi_image.py`) |
| `0x000200..0x0021ff` | 8 KiB | vendor SRAM bootstrap (PLL/DDR calibration, exception vector table). Must stay byte-identical; `spi_image.py` refuses an unknown bootstrap hash |
| `0x002200..` | N | the BootROM load block = the SDK image (ROM sector 17 onwards) |
| after the load block, 4 KiB aligned | M | **overlay segments** (only for images that need them, e.g. LVGL); see below |
| `image_end..0x1fbfff` | the rest | **free for application data**, `image_end` = `apx_spi_image_end()` |
| `0x1fc000` | 4 KiB | overlay table (magic `APXO`); erased when the image has no overlays, then also free |
| `0x1fd000` | 4 KiB | unused |
| `0x1fe000` | 4 KiB | diagnostic log of USB-launched runs, **only in `APX_SPI_TRACE=1` builds** |
| `0x1ff000` | 4 KiB | diagnostic log of SPI boots, **only in `APX_SPI_TRACE=1` builds** |

So the hard ceiling for "image + data" is 2048 - 8.5 - (up to 16 for table and logs) = about 2020 KiB.

What each shipped example leaves you (measured from the packed images; `ends` is `apx_spi_image_end()`):

| Example | Load block | Overlays | Ends at | Free for data | Wasted zeros inside the load block |
| --- | --- | --- | --- | --- | --- |
| camera-demo | 1152 KiB | - | `0x122000` | 872 KiB | 548 KiB |
| gpio-demo | 1112 KiB | - | `0x119000` | 908 KiB | ~556 KiB |
| jpeg-demo / microphone-demo | 1192 KiB | - | `0x12d000` | 828 KiB | ~554 KiB |
| lvgl-demo (widgets) | 52 KiB | 937 KiB | `0x0fb000` | 1028 KiB | 0 |

The free area is **not blank**. It still holds the stock firmware's old data (the images are built on the
stock backup `research/spi-boot/original.bin`), 70-75 % of it non-`0xff`. That includes the vendor photo area
at `0x1ba000..0x1befff`, which only differs between `original.bin` and `flash.bin.golden`. Nothing on the camera
reads any of it; erase before you write.

## Using the free area from the SDK

```c
#include <appotech/spi.h>
apx_spi_init();                              // once; takes the pins from the display/SD while it runs
uint32_t start = apx_spi_image_end();        // 4 KiB aligned; 0 means the flash could not be read
// read
apx_spi_read(start, buffer, length);
// write: erase whole sectors first, program at most one page (<=256 B, not crossing a 256 B boundary) at a time
apx_spi_erase_sector(start);                 // address must be 4 KiB aligned; waits for completion (<=5 s)
apx_spi_program(start, page, 256);
```

* Stay inside `[apx_spi_image_end(), 0x1fc000)` (plus `0x1fc000` if the image has no overlay and `0x1fd000`).
  Never write sector 0 or anything below `0x2200`: that is the header and the bootstrap, and a bad header means
  the camera only boots through the BootROM loader again (recoverable, see `spi-boot-recovery.md`, but avoid it).
* The driver is synchronous, cooperative and not re-entrant (`APX_SPI_BUSY`); it keeps the watchdog fed while it waits.
  A power cut between erase and program loses that sector; journal anything that matters (two slots and a
  sequence number is enough).
* Endurance of NOR is on the order of 100 000 erases per sector: rotate sectors for frequently written state.
* From the host: `apxtool debug flash read --address A --length N OUT` reads any range. Writes are
  `FlashClient.erase_sector(addr)` / `program_page(addr, data)` in `apxtool/sdk_flash.py` (they send the `APXF`
  marker the firmware requires for destructive SPI commands). That API is read from the source; the
  erase/program/read-back sequence itself was last exercised on hardware through `flash install`.

## Two ways your data gets destroyed

1. **Installing an image rewrites every sector that differs from the packed file.** `apxtool debug flash install`
   and `rom-rescue restore` take a full 2 MiB image; data you wrote into the free area is overwritten with the
   image's bytes (the stock leftovers). Either put the data into the image before installing,
   ```python
   image = bytearray(Path("camera-demo.spi.bin").read_bytes())
   image[0x140000:0x140000 + len(blob)] = blob      # inside the free area; keep clear of 0x1fc000 and above
   ```
   (install writes only sectors that differ from the chip, so unchanged data is not rewritten), or dump the region first
   (`debug flash read`) and write it back afterwards with the two calls above.
2. **Diagnostic builds use the last two sectors.** `APX_SPI_TRACE=1` erases `0x1fe000` and `0x1ff000` at every
   boot. Keep your data out of them if you ever want those builds.

## Why images are bigger than the code

The BootROM loads sectors 17..17+N into **consecutive** DDR addresses from `0x02180000`. The linker (`linker/ram.ld`)
puts the main code at `0x02180000` but the big optional blocks (camera, audio, JPEG, FatFs tables) in the
"extra RAM" bank at `0x02220000`, because on a USB launch those are the reserved ranges. In an SPI image the
packer fills everything between them with zeros: about **550 KiB of flash holding nothing** in camera-demo,
gpio-demo, jpeg-demo and microphone-demo. `flash_map.py` reports it as "all-zero stretches".

## Getting more flash for the app

Ordered by payoff. 1 and 2 are done for LVGL, not for the others.

1. **Keep small segments next to the main code** (done for LVGL). `Makefile` builds `build/ram-compact.ld` from
   `linker/ram.ld` for `EXAMPLE=lvgl-demo`, dropping the `.peripheral_*` sections so UART/ADC/battery/SPI drivers
   land in the main block instead of a 9 KB segment at `0x02220000`. That alone took the LVGL image from
   about 1.6 MB to 1.0 MB. Any example whose only extra-RAM content is small can do the same.
2. **Overlay segments** (done for the codec-RAM bank, `0x022e0000..`). `tools/spi_image.py` stores any segment that
   starts at or above `0x022e0000` after the load block, writes a CRC-checked table at `0x1fc000`, and the
   application calls `apx_spi_overlay_load()` first thing (`examples/lvgl-demo/main.c`) to copy it into RAM
   (about a second for 937 KiB, not timed precisely; USB and the watchdog stay serviced; USB launches skip it because the launcher already loaded
   everything). To reclaim the 550 KiB gap of the other examples the same mechanism has to be extended to the extra-RAM
   bank, with one catch: everything run before the copy must live in the main block, including the SPI driver and
   the weak hooks `apx_maintenance()` calls (`apx_audio_poll`, `apx_camera_poll`, ...). Do the copy before anything
   that could call into the overlay, and make `spi.c`'s wait loops skip those hooks until it is done. Not implemented.
3. **Compress the overlay.** LVGL's 620 KB of code and 330 KB of tables should shrink by roughly half with an LZ
   scheme; the loader needs a decompressor of about 1 KB and the packer a matching compressor. Not implemented;
   estimated 400-500 KiB saved on LVGL.
4. **Do not link what you do not use.** Most of the LVGL overlay is demo assets and unused widgets/fonts: set the
   demos and fonts you do not need to 0 in `include/lv_conf.h`. Keep FatFs out of programs that do not use the SD card.
5. **Stream assets instead of copying them.** Read-only images, fonts and audio can live in the free area and be read
   on demand with `apx_spi_read` instead of being part of RAM-resident overlays.
6. **Use the SD card for bulk data.** It is the camera's real mass storage; flash is for configuration and small assets.
7. **Reclaim fixed sectors.** `0x1fd000` is always free, `0x1fc000` is free in images without overlays, and
   `0x1fe000/0x1ff000` in production builds.

## Related

* `spi-boot-recovery.md`: how the image is packed and installed, recovery without a programmer, overlay and timing findings.
* `apxtool/src/apxtool/spi_boot.py`: packer (constants `LOAD_BASE`, `MAX_LOAD_END`, `OVERLAY_TABLE`).
* `src/spi_overlay.c`: overlay loader and `apx_spi_image_end()`; `include/appotech/spi.h`: the flash API and map constants.
