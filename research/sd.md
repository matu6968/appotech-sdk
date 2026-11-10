# JT5295 SD and FatFs evidence

This records the initial read-only milestone. [SD write/format evidence](sd-write.md)
describes the subsequent transmit DMA, writable stdio and volume lifecycle implementation.

The independent SD controller uses SPR `0x9880..0x98a4`. All SD commands,
receive DMA and filesystem operations execute SDK code. No stock or ROM
storage routines are called. SPI and card write/erase commands are absent.

## Sources and controller trace

Raw native little-endian instructions in `spi-main.disasm` take precedence
over the vendor program's generic OpenRISC delay-slot model. Main SPI offset
`0x2200` maps to `0x02000000`; address-to-file bias is `0x01ffde00`.
The loaded golden image hash is recorded in the existing evidence ledger.
`AnnotateSD.java` adds labels/comments in Ghidra without replacing its language
or existing analysis. The original program has limited function analysis;
the initial string-reference queries found no references.

| Vendor address | Raw instruction evidence | SDK interpretation |
| --- | --- | --- |
| `02050a4c` | Gate 22, SPR 9280 bit 12, mux selector 7, GPIO masks, 9880/9884/9888 | Controller initialization |
| `02050c0c` | 9898=command\|40, 989c=argument, 988c=2000, 9884=mode | Command start, R1/R2/R3/R6/R7 responses |
| `02050d58` | 9880 bit 2 | One/four-bit controller selection (stock never selects four bits; the SDK does on boards that wire it, see the four-bit section) |
| `02050d9c` | 9890=CPU address, 9894=byte count, event8000/10 | Arm receive DMA |
| `02050eb8` | Wait 9880 bit 15, acknowledge 9884 bit 15 | Receive completion |
| `02050f48` | Read status mask 00600000 equals 00400000 | Receive data validation |
| `02050fa8` | Return pointer 2000 | Long response SRAM |
| `020510a8` | Literal 08954400 and divisors | Nominal 144 MHz SD clock source |
| `02032478` | CMD0/8/55/41/2/3/9/7/16 and CSD fields | Card initialization and capacity |
| `020322dc` | SDSC byte address vs SDHC sector address; CMD18 streaming | Stock read path; SDK uses CMD17 single blocks |
| `02022d14` | SPR 181c then 1814 with 16-byte line counts | Receive DMA cache maintenance |

The [SD Association simplified specification](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver6.00.pdf)
defines the CSD/OCR fields and SDSC/SDHC addressing used by the SDK.
The 16-byte R2 response reverses bytes within each 32-bit word; the SDK
normalizes CID/CSD to wire order. CMD_DONE is 9880 bit 17 and command timeout
is bit 19 (physically observed after disabling the board's required PA2 output).
R1 errors and R6 error bits are checked. R3 has no CRC.

## Board-specific pin finding

`sd-fresh-pin-registers.json` records stock GPIO 8040=123b. A controlled
change to 123d, following the generic SD initialization, made CMD8 time out
(9880=5a0043 and response 0). Restoring 1239 recovered CMD8 immediately with
response 155 and status 520043. PA2 therefore remains an output driven low;
its electrical role is unconfirmed, possibly card enable/power. The driver
preserves this board requirement and configures only the one-bit route.
The first four-bit-pin attempt and the subsequent uncorrected one-bit attempt
are retained as failed evidence, not accepted configurations.

Clock and speed: see "Clock, speed and streaming (measured 2026-11-08)" below, which corrects the earlier reading of the divider
(the SD clock is 144 MHz/(divider+1) with an 8-bit register, not 144 MHz/(2*(divider+1))). Clock 9888 reads back zero, so it is
write-only. Four-bit operation: see the last section. Card detection is command-based; close handles and remount after removal/read failure.
There is no hotplug GPIO driver yet.

## FatFs integration

User-supplied FatFs R0.16 (`FF_DEFINED=80386`) is compiled into `libfatfs.a`.
`fatfs-user-config.json` preserves the original configuration hash/settings.
SDK changes: CODE_PAGE=0, UTF-8 API, LFN_BUF=768, heap LFN buffers (mode 3),
and fixed timestamps until RTC support. exFAT, mkfs, fastseek, label APIs,
relative paths and one-object locking remain enabled. `f_setcp()` selects
the OEM short-name codepage; LFN paths remain UTF-8. Single cooperative task,
no mutex/TLS/OS integration. Allocation uses picolibc malloc/free and the
existing 16 KiB heap. Failed LFN allocation is reported by FatFs.

The [FatFs configuration documentation](https://elm-chan.org/fsw/ff/doc/config.html)
and [porting documentation](https://elm-chan.org/fsw/ff/doc/appnote.html)
describe these options and diskio/allocation hooks. The supplied skeleton
`diskio.c` and OS example `ffsystem.c` are not compiled; SDK glue supplies
diskio and heap hooks. `FF_FS_READONLY` stays 0 so the requested APIs are
available to future firmware. Physical disk status is always STA_PROTECT,
and disk_write always returns RES_WRPRT. f_mkfs/f_setlabel/write opens reject
the card without issuing storage write commands. Model formatting happens
only on disposable host RAM images.

Picolibc FILE streams are available through `apx_fopen(path,"rb")` and use
FatFs read/seek/close callbacks. They support standard fread/fseek/fclose and
64-bit file offsets. Write modes fail with EROFS. The direct FatFs API remains
available for applications that need directory/stat/label access.

All-codepage tables occupy 483036 bytes in `.fatfs_tables` at 02220000,
reserved through 022dffff. The region is uploaded, hash checked, preflighted,
protected by APXD and released only after acknowledged BootROM recovery.
USB-only and display-only applications retain their original reservations.
`sd-footprint.json` records exact final section sizes.

## Validation boundaries

The physical card is SDSC, 3862528 sectors (1977614336 bytes), FAT16 with
64-sector clusters. Sector 0 from the first independent probe matched stock
USB MSC byte for byte. Stock SCSI READ(10) captures in `sd-stock-baseline.json`
cover sectors 0..63 and 2048..2055. Five complete existing files were read
through both Linux/stock MSC and SDK FatFs and matched SHA-256 hashes.
Host-mounted partitions were unmounted before takeover.

`test_sd_model.py` tests the real SD driver and diskio glue with an independent
controller model: SDSC/SDHC capacity and addressing, unaligned caller buffers,
DMA guards, bounds, controller IRQ disable/preserved CPU masks, CRC, absent card, stuck command/data, recovery,
and write rejection. Invalid SDSC capacities that would overflow the 32-bit
command byte address are rejected before card selection. `test_fatfs_model.py` runs actual FatFs and FILE integration
with sanitizers on FAT12/16/32/exFAT RAM images, UTF-8 Japanese/emoji/LFN,
codepages, labels, relative paths, locks, seeks and write protection.
SDHC/SDXC and non-FAT16 filesystems are model-tested; only this SDSC/FAT16
card has physical validation (at the time of this first milestone; streaming, writes and the four-bit bus came later, see below). MMC, SDUC and format
execution on a real card are not implemented.

Final live stress/recovery/preservation results are in `sd-acceptance.json`.
Card preservation claims cover captured sectors and file hashes; they are
not a full 2 GB card hash. SPI preservation uses a complete 2 MiB comparison.

## Clock, speed and streaming (measured 2026-11-08, Silvergear, SanDisk SU02G 2 GB SDSC)

Everything below was measured on the camera with `examples/sd-bench` (read-only: CMD13/CMD17/CMD18 only; `EXAMPLE_DEFS=-DAPX_BENCH_RAW=1` for the
raw clock probe) and decoded from the Silvergear stock disassembly (`research/silvergear/stock-app.disasm`).

**The divider.** SPR 0x9888 is **8 bits wide** and the SD clock is **144 MHz / (divider + 1)**. Evidence: a raw CMD13 round trip takes
0.778 us per divider step for dividers 2..255 (112 SD clocks at 144 MHz); dividers that differ by 256 give identical times (359 = 103,
300 = 44, 267 = 11, 600 = 88, 256 = 0); the new value takes effect for the very next command. The absolute clock is confirmed independently by
streaming throughput, which reaches 97.6% of the 1-bit bus limit at divider 5 (2.92 of 2.99 MB/s) and 95% at divider 2 (5.71 of 5.98 MB/s).
- The SDK used `144e6/(2*f) - 1`, which is wrong by 2: its "24 MHz" (divider 2) was **48 MHz in default-speed mode**, out of spec for a card rated 25 MHz
  (this card's CSD TRAN_SPEED is 0x32). It worked on the cards tried.
- Both the SDK and the stock firmware write 359 for card identification, which the 8-bit register truncates to 103: **1.39 MHz**, not the 400 kHz the
  standard allows (stock `0x2074cf0`: `l.addi r3,r0,359`). The slowest reachable clock is divider 255 = 562.5 kHz, which the driver now uses.
- Stock's operating clocks (`0x2075230`, called as `(48000000, 24000000)` from `0x2038ffc`): `divider = 144000000/f - 1`, so 24 MHz = 5 and 48 MHz = 2.

**High speed (CMD6).** Stock `0x207514c` reads a 64-byte status twice: CMD6 `0x00FFFF01` (check) then `0x80FFFF01` (switch), each requiring DATA_DONE, CONTROL
bit 11 clear and the low nibble of status byte 16 equal to 1. On success `0x2075270` sets CONTROL bit 31, then bit 3, and writes divider 2 (48 MHz); on
failure it clears bit 3 and uses divider 5. The driver does the same (a DDR buffer instead of stock's SRAM 0x1e40, which overlaps the Action bootstrap's saved
stack page). **This card accepted it** (CSD command class 10 set) and ran at 48 MHz with no errors over 5 access patterns. The effect of CONTROL bits 31/3 themselves
was not observable on this card: reads were identical with and without them at 24, 36 and 48 MHz.

**DMA.** One arm = one 512-byte block (`DMA_LENGTH` > 512 sets the error flag on every arm and corrupts the data; the area after the buffer stayed untouched).
The arm is stock `0x2074f24`: address, acknowledge DATA_DONE, length, EVENT |= 0x10, CONTROL &= ~2. Stock arms the receive **before** sending CMD17/CMD18.

**Streaming.** Stock `0x20389c0` keeps one open-ended CMD18 running between calls (state byte: 1 reading, 2 writing, 3 none; next-sector word) and only re-arms the
DMA for a sequential continuation; anything else first sends CMD12, waits for DAT0 (CONTROL bit 1 / CARD_READY bit 20), then CMD13 until the state is "tran".
The driver now does the same, but sends an isolated single sector as plain CMD17 (a stream there would cost an extra CMD12 round trip). Raw CMD17 rate is
0.7-0.8 MB/s at every clock (command overhead, not bandwidth); a CMD18 stream scales with the clock.

**Status bits 22:21 are a WRITE status, not a read check.** Stock `0x20750d0`: for reads only CONTROL bit 16 is an error; for writes the bits 22:21 must equal
`10b` (the card's CRC-status token, "data accepted"). On reads they are noise, which is why the SDK saw them vary with correct data. **The SDK's write path
checks only bit 16**; the write phase should also require `(CONTROL >> 21 & 3) == 2` after each block.

**Driver read rates after the change (SU02G, high speed 48 MHz, verified byte-identical to a raw CMD17 reference at 12 MHz):** 512 sectors in one call 4.6 MB/s;
512 single-sector sequential calls 4.3 MB/s; 64 calls of 8 sectors 4.6 MB/s; 512 single random sectors 0.77 MB/s; an unaligned buffer (bounce + copy) 2.7 MB/s.
Before: about 0.4-0.6 MB/s for every pattern. USB mass storage over this: 0.59 -> 1.88 MB/s sequential (now limited by the USB FIFO path).

**Filesystem path (FatFs, via the debug link's `sd get`):** all 14 files on the card (12 photos and 2 AVIs, 5,336,781 bytes) fetched with the old driver (SPI image) and with
the new one (RAM launch) are **bit-identical**; the whole fetch took 26.4 s -> 11.3 s, dominated by one `apxtool` process start per file. The card's counters after the new run:
14,328 reads, 1,308 streams, 0 writes, 0 errors, 0 retries, 48 MHz, high speed.

**The model** (`tests/sd_model.c`) covers the stream (reuse across calls, stop on write/sync/clock change/shutdown, mid-stream error retry), CMD6 (success, "not selectable", CRC
error, no class 10), the 8-bit divider and the identification clock. It was mutation-checked with twelve deliberate driver bugs; all meaningful ones fail it (the one equivalent
mutant is shutdown clearing the stream flag when init also clears it).

**Nine cards, Silvergear, RAM launch, read-only (2026-11-08).** All nine accepted CMD6 and ran at 48 MHz in high speed with 0 errors and 0 retries, and every read
pattern was byte-identical to a raw CMD17 reference at 12 MHz (taken from a window that holds real data: `sd-bench` picks one). Rates in MB/s (steady-state; the
first of three repeats is sometimes ~7% slower), "1x512" = one 512-sector call, "seq" = 512 single-sector sequential calls, "8x" = calls of 8 sectors, "rand" = random single sectors:

| Card (CID) | Size | 1x512 | seq | 8x | rand | End of card, old driver | Fixed driver |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SanDisk 2 GB SDSC (MID 03, SU02G) | 1.8 GiB | 5.10 | 4.89 | 5.08 | 0.86 | pass | pass |
| Toshiba EXCERIA 16 GB (MID 02, SA16G) | 14.4 GiB | 5.12 | 4.92 | 5.11 | 1.26 | **fail** | pass |
| SanDisk Ultra 16 GB (MID 03, SC16G) | 14.8 GiB | 4.83 | 4.56 | 5.04 | 1.38 | pass | not re-run |
| Philips 32 GB (MID 6f, SDABC) | 29.1 GiB | 4.85 | 4.58 | 5.12 | 2.30 (see below) | pass | not re-run |
| Samsung 64 GB (MID 1b, JC1S5) | 59.7 GiB | 5.12 | 4.94 | 5.11 | 1.65 | **fail** | pass |
| Samsung 64 GB (MID 1b, EC1S5) | 59.7 GiB | 5.09 | 4.94 | 5.11 | 1.66 | **fail** | pass |
| PNY PRO Elite 64 GB (MID 27 "PH" = Phison, SD64G) | 58.3 GiB | 4.82 | 4.56 | 5.10 | 1.41 | pass | not re-run |
| Samsung PRO Plus 128 GB (MID 1b, FD4Q9) | 119.4 GiB | 5.14 | 4.91 | 5.11 | 2.88 | pass | not re-run |

The 1-bit bus limit at 48 MHz is 5.98 MB/s; the best cases reach 86%. The unaligned-buffer path (bounce + copy) is CPU-bound at 2.85 MB/s on every card.

**Found by the end-of-card test: a stream must not run into the last block.** `sd-bench` reads the final 8 sectors through the driver and through raw CMD17. With the first
CMD18 driver, the Toshiba and both Samsung 64 GB cards failed: the driver read succeeded, then the card was dead (every raw command failed). Mechanism (reproduced in the host
model, **not observed on the wire**): a stream that reaches the card's last block is ended by the card, and some cards flag OUT_OF_RANGE in the R1 of the CMD12 that stops it; the driver
took that for a failure and shut the card down. A Linux host probes the last sector (backup GPT), so MSC mode would have hit this. Fix: `read_block()` reads the card's last
block with CMD17 and never as part of a stream. Confirmed on all three failing cards; the model test fails with the old behaviour (a first attempt at the test failed for
an unrelated reason, an error counter that earlier steps had raised on purpose, and was corrected to compare against a baseline before it was trusted).

**Run-to-run variation is large for some cards, so one run per card does not rank cards.** The Philips card, run right after insertion, gave 3.2-3.9 MB/s and **63 KB/s** random; the same
card, same data, minutes later gave 4.6-5.1 MB/s and 2.3 MB/s random. The cause is not known (a card still busy after power-up is a guess). `sd-bench` now runs each pattern three times and prints all three.

**Accidental hot-removal data point:** a card pulled while the bench was reading made the driver report errors and "not ready"; it did not hang and the program finished.
It is not a hot-removal test (no controlled removal, the camera was only reading).

Not measured at that time: cards that do not support high speed (CMD6 is skipped when CSD command class 10 is clear), UHS-I, the Action camera (done later, see the four-bit section), four-bit mode (done, see below).

**`service()` frequency matters under TinyUSB (2026-11-08).** The driver called `service()` (USB poll, buttons, watchdog) on every spin of a status wait and once per sector. With TinyUSB a poll is about 11-12 us (legacy 3.1 us), so it taxed every
sector. Now the waits spin on the status register and service every 256th spin, and the per-sector service runs every 8th sector. Silvergear 2 GB SU02G, sd-bench: 512 sectors in one call 5.27 MB/s (was 4.60), 512 x 1 sequential 4.64 (was 4.10), 64 x 8 5.19,
errors 0, byte-identical to the raw reference. MSC (read-only SD) sequential read 1.96 MB/s, `tools/test_usb_msc_sd.py` 16/16.

**Read-ahead (2026-11-08).** While a CMD18 stream is open the driver arms the receive DMA for the next sector as soon as the current one is in, into one of two driver blocks (`prefetch_buf[2][512]`),
so the card sends it while the caller does other work; the next call waits for it (usually already there) and copies it out. A stream is stopped between blocks (the pending block is finished first, then CMD12).
Never for the card's last block (see the end-of-card note above). **Timing matters:** the first version armed the next receive after copying the finished block out (about 10 us after DATA_DONE); throughput fell from 5.3 to
3.0 MB/s, two block times per sector, so the controller evidently misses the start of a block that begins before the DMA is armed (the card starts the next block within about a microsecond). Arming first, then copying, restores 5.3 MB/s
(`sd-bench`, SU02G 2 GB, byte-identical to the raw reference, end-of-card test passes). The point is mass storage: SD-backed MSC sequential read went 1.69 -> 2.54 MB/s (`test_usb_msc_sd.py`, 16/16), because the card's 98 us per sector
now overlaps the USB work instead of adding to it. Model (`tests/sd_model.c`): blocks arrive a few status reads after arming, CMD12 asserts that no receive is armed, errors can be placed on a chosen delivery (mid-call and in a read-ahead block found by the next call);
mutants (no wait before CMD12, read-ahead of the last block, ignoring the error flag, skipping the copy) are all caught. Only the 2 GB SDSC card was run with it; the other eight cards have not been re-run, the arm-before-copy timing could differ on them.

## Write path: CMD25 streaming (2026-11-08)

The owner approved writing to the card in the Silvergear (SanDisk SU02G, 2 GB SDSC) for this work. `apx_sd_write()` used one CMD24 per sector (about 258 KB/s). It now mirrors the read stream:

- **Open-ended CMD25.** A write of more than one sector, or one that continues the previous write, sends CMD25 once and then, per sector, only: arm the transmit DMA (address, acknowledge, length 512, EVENT |= 0x30), wait DATA_DONE, check the
  data-response token, wait for the card to let go of DAT0 (CONTROL bit 1 set while polling bit 20, the same wait stock uses after CMD12; **no CMD13 per block**, as in stock `02038ae0..02038b24`). The stream stays open between calls
  (`wstream`), the way the read stream does, and is closed (CMD12, then the ready wait and CMD13 until the card is back in the transfer state) by: any read, `apx_sd_sync()`, a clock change, a write that does not continue it, shutdown, and
  `apx_sd_poll()` after 50 ms without a write (`apx_maintenance()` calls it when the SD driver is linked; it stays out of the way while a public SD call is running). An isolated sector is still one CMD24 followed by the ready wait and CMD13.
- **The data-response token is now checked.** Stock `020750d0` accepts a write only if `(CONTROL & 0x600000) == 0x400000` (bits 22:21 = 10b, the card's "010 = accepted") and bit 16 is clear; the old code looked at bit 16 alone. Confirmed on the card: every
  one of 16,616 written blocks in `sd-write-test` carried the accepted token (`last_write_status` in the info structure shows the last one), and none was refused.
- DMA straight from the caller's buffer when it is 16-byte aligned (one `apx_dma_clean` for the whole call), else through the bounce block. A replay after a card reinitialisation cleans the lines again.
- Errors: a block that fails (timeout, bit 16, wrong token) shuts the card down and is replayed from that sector after reinitialisation, at most twice, as before; write protection (R1 bit 26) is never retried.
- Model (`tests/sd_model.c`): CMD25 with data-response tokens and busy after every block, an assertion that no block is armed (and no data command issued after CMD12) while the card is busy, CMD13 answering "receiving data" while a stream is open (a driver
  that polled it would hang), token errors placed on a chosen block, the idle poll (not early, not while a call runs even when maintenance is slow), and the stop paths. Ten mutants (no token check, no busy wait, reads not closing the stream, sync not closing it,
  poll never/always closing, no sequential continuation, no ready wait after CMD12, stale next-sector, no re-clean on replay) are all caught.
- Hardware (`examples/sd-write-test`, run from RAM): raw writes in the 62-sector gap between the MBR and the first partition (single, multi-sector, sequential singles, unaligned source, interleaved with reads), restored and compared (MBR and gap
  byte-identical, an external backup of sectors 0..631 taken over the debug link beforehand showed one changed sector: a deleted directory slot reused for the scratch file); then an 8 MiB scratch file through FatFs in 512 B, 4 KiB and 64 KiB writes (1.47, 1.92 and 1.17 MB/s), read back and verified
  (2.03 MB/s), deleted. 22/22 steps, no errors or retries, 16,616 sector writes in 20 write streams. The rates are this old card's (about 250 us per block of programming); other cards have not been tried.
- USB: `apx_usb_msc_use_sd_writable()`; `tools/test_usb_msc_sd_rw.py` 18/18 (raw gap writes, a 16 MiB file with fsync at 1.29 MB/s, read back with the same SHA-256 at 2.62 MB/s, 40 small files, deleted, `fsck.vfat -n` clean, MBR and gap unchanged).

## Write path on all eight cards, and hot removal (2026-11-08)

`examples/sd-card-test` (mode 0) runs, once per inserted card and twice per card in this session, the raw tests in the all-zero part of the gap after the MBR (single sectors, 1/2/5/16/62-sector CMD25 writes, an unaligned
source, sequential single sectors, writes interleaved with reads; restored and compared) and an 8 MiB scratch file through FatFs (512 B, 4 KiB, 64 KiB writes, read back and verified, deleted). The owner approved it for his test cards. **All eight cards passed, with no
errors, retries or refused data-response tokens**:

| Card (CID name) | Size | Result | File write KB/s (512 B / 4 KiB / 64 KiB) | Read-back KB/s |
| --- | --- | --- | --- | --- |
| SanDisk 2 GB SDSC (SU02G) | 1.84 GiB | 8/8 | 1793 / 1161 / 1855 | 2067 |
| Toshiba EXCERIA 16 GB (SA16G) | 14.4 GiB | 8/8 (x7 runs; one first run after insertion failed, see below) | 1950 / 2134 / 2131 | 2050 |
| SanDisk Ultra 16 GB (SC16G) | 14.8 GiB | 8/8 x2 | 2055 / 2208 / 2224 | 2058 |
| Philips 32 GB (SDABC) | 29.1 GiB | 8/8 x2 | 1636 / 2090 / 2108 | 2025 |
| Samsung EVO Plus 64 GB (EC1S5) | 59.7 GiB | 8/8 (log of the details lost, result line seen) | - | - |
| Samsung PRO Endurance 64 GB (JC1S5) | 59.7 GiB | 8/8 x2 | 953 / 1078 / 958 (slowest) | 1874 |
| PNY PRO Elite 64 GB (SD64G) | 58.3 GiB | 8/8 x2 | 1872 / 2058 / 2088 | 2024 |
| Samsung PRO Plus 128 GB (FD4Q9) | 119.4 GiB | 8/8 x2 | 1923 / 2118 / 2094 | 2028 |

The write rates sit close to the 1-bit bus and per-block busy limits (about 2 MB/s); the Samsung PRO Endurance is half of that.

**One failure that did not repeat.** The first run on the Toshiba card, directly after it was inserted (hot-swapped from the previous card), failed every raw step after the second write (the data of the 5-sector write and everything later did not persist, no error, no bad
token) while the file test that followed worked; the gap was left with two sectors of test data (put back to zeros afterwards, its content before the test had been logged as 62 zero sectors). The same card then passed 14 more runs (including a rerun in the diagnostic mode and six
back-to-back repeats) and no other card showed it. Cause (the owner's explanation): the card was being inserted slowly, so its contacts were still bouncing while the driver initialised it. The driver reported nothing because every command it issued was answered; a card that is still seating can accept init and then miss data. Inserting the card firmly avoids it; the driver does not currently re-verify a freshly initialised card. `sd-card-test` now prints what it read back when a verify fails, to catch it next time.

**Hot removal** (`sd-card-test` mode 1, 5 minutes, the owner pulled and reinserted the Samsung PRO Plus 128 GB 41 times, in all phases: idle sector reads, CMD18 read stream, CMD25 write stream into the gap): every pull was detected by the driver at once (the failing call took 11-525 ms,
error -2 or -1 depending on the command in flight; the card is shut down and the call returns), the driver never hung, the watchdog never fired, and the card was re-initialised after every reinsertion (41 of 41, same CID; init is retried every 333 ms). At the end every gap sector held one complete
generation (none torn, none garbage). USB side (`tools/test_usb_msc_hotpull.py`, the host writes the gap through the camera's writable disk): the first version recovered **never**: after the first pull the camera kept answering every write instantly with an error because the SD driver stays shut down until
something asks the disk "ready", which a host that keeps writing never does (472 errors, 0 recoveries). Fixed in `msc_disk.c`: every READ/WRITE(10) first asks the disk's ready hook (which retries the card init at most once a second); a missing medium is answered "not ready, medium not present" (the host reports
ENOMEDIUM and stops), a card that fails under a command gives MEDIUM ERROR (write error 0x0c / unrecovered read 0x11), and the first command after the medium returns is refused once with UNIT ATTENTION "medium may have changed". Result of the 4-minute rerun (25 pulls): 2046 generations completed, **25 of 25
recoveries** (3-9 s each, mostly the time to reinsert), the device node survived, no torn sector. A pulled card with the disk idle: the host reads "No medium found" and the disk is readable again about a second after reinsertion, without replugging.
Not tested: pulling the card while the HOST has a filesystem mounted and is writing files (the filesystem, not the camera, decides what survives), pulling the USB cable, other cards for the hot-pull runs.

## Four-bit bus and direct multi-sector DMA (2026-11-10)

**Stock never runs four bits.** Both stock images contain a four-bit branch that no caller reaches: the width routine (Action `0x02050d58`, Silvergear `0x02074ee0`) sets CONTROL bit 2 only for argument 1,
its two callers are inside card init (`0x2032478`) and every call passes 0, and stock even sends ACMD6 with argument 0. That branch is nevertheless the only description of the pins: controller init (`0x02050a4c`) routes
**PA0..PA3 and PE10/PE11** for the wide bus (peripheral bit `0x8100`/`0x8110`, pull-up `0x8080`/`0x8090`, no pull-down `0x80c0`/`0x80d0`, input `0x8040`/`0x8050`, PA2 still an output driven low). DAT1..DAT3 are therefore
taken to be PA3 and PE10/PE11 (an inference). On the Silvergear these pads are printer motor phases (PA3, PE10, PE11) and the cameras' power-down controls (PE10/PE11), so that board stays one-bit
(`APX_SD_BUS_WIDTH_MAX` in `board.h`: Action Vision 4, Silvergear 1).

**Negotiation (`negotiate_wide()` in `sd.c`).** After the card is selected and in the transfer state, one line: (1) read the 64-byte SD status (ACMD13) on one line; (2) route the pads, send ACMD6 with argument 2, set CONTROL bit 2;
(3) read the SD status again. The first byte holds DAT_BUS_WIDTH (bits 7:6, 00 -> 10) and the other 63 bytes are static, so equal bytes (and the width field) prove that all four lines deliver data. Any failure (refused ACMD6, CRC error or timeout
on either read, a mismatch) makes init restart on one line (CMD0 returns the card to one bit; the restart's shutdown clears CONTROL bit 2 and puts the pads back as they were) and blocks four-bit for that card (by CID) until another card appears.
`struct apx_sd_info` reports `bus_width`, `bus_probe` (enum `apx_sd_wide`) and `bus_probe_detail` (for a mismatch: byte index + 1, the four-bit byte, the one-bit byte). SCR is not read: a card that refuses ACMD6 simply stays on one line.

**Hardware (Action Vision, RAM launches, 2026-11-10).** The Action PCB does wire DAT1..DAT3: the probe passed on the SanDisk SU02G 2 GB and on the Samsung PRO Plus 128 GB (FD4Q9), reads are byte-identical to the one-bit reads (same window word sum),
0 errors. The write suite (`examples/sd-card-test`, mode 0) passed 8/8 steps with 0 errors, retries and bad data-response tokens at four bits on the FD4Q9 (file write 512 B / 4 KiB / 64 KiB chunks 2.6 / 2.9 / 2.8 MB/s against 1.9 / 2.1 / 2.1 MB/s on one bit,
read-back 2.64 against 2.05 MB/s). Raw four-bit CMD24/CMD25 writes of 1, 2, 7 and 62 sectors, patterned and zero, read back identically (gap sectors only, restored). The card as a USB disk (read-only SD mode of `usb-modes`): the host's reads of five regions, including the last
sectors of the card and the first 10,400 sectors, are identical to the debug-link reference; 2.13 MB/s (USB-bound).

**The limit was the copy, not the bus.** The first four-bit run gained almost nothing (SU02G and FD4Q9 both flat at about 6.0 MB/s from 24 MHz up), although a clock sweep showed the bus did scale at 6 and 12 MHz. A raw stock-style CMD18 stream (arm the DMA right after each block, no copy) reached
**21.2 MB/s at four bits / 48 MHz** (11.1 at 24 MHz, 16.3 at 36 MHz), so the controller, the SoC and the card were not the limit. The driver's read-ahead received every block into a driver buffer and copied it out: about 70 us per 512-byte sector from freshly DMA'd (uncached) DDR, a ceiling of about 6 MB/s at any clock.
Fix: inside a multi-sector call into a 16-byte aligned caller buffer, the next DMA is armed straight into the caller's next sector (`start_prefetch(next_target)`, `prefetch_target`); the driver blocks serve only the last block of a call (so the next sequential call finds its data waiting) and callers that read one sector at a time.
Samsung FD4Q9, four bits, 48 MHz, `examples/sd-bench`: 1 call x 512 sectors **5.8 -> 20.6 MB/s**, 64 calls x 8 sectors 6.1 -> 16.2 MB/s, clock sweep 6/12/24/36/48 MHz = 2.8/5.6/9.6/13.8/16.3 MB/s; every sector identical to the reference. Still copy-bound: 512 calls of one sector 5.8 MB/s and an unaligned destination
(bounce block + copy) 3.2 MB/s.

**Model and mutants.** `tests/sd_model.c` models ACMD6/ACMD13, a card width state, asserts that CONTROL bit 2 and the card width agree on every data transfer and that CONTROL bit 2 is still clear when the card changes width, and covers eight failure modes
(ACMD6 refused, DAT1/DAT2/DAT3 dead, CRC error and timeout on the four-bit status read, the one-bit reference read failing, DAT_BUS_WIDTH still 00), pad restore, the per-card latch (a replaced card is tried again), where each delivered block lands (the caller's buffer or a driver block),
and the board limit (`tools/test_sd_model.py` builds Action Vision and Silvergear). Mutants killed: bit 2 set before ACMD6, bit 2 never set, no compare, only byte 0 compared, width field ignored, pads kept after shutdown, latch ignored or never cleared, no restart, wrong ACMD6 argument, bit 2 kept after shutdown,
direct DMA disabled, armed one block too many, always copy, never copy, wrong destination. (Cleaning up the pads and bit 2 again in the failure path was found to be redundant with the restart's shutdown and was removed.)

**Not done:** other cards' four-bit write suites, four bits on the Silvergear (PA3, PE10 and PE11 belong to the printer and the cameras there), CMD23/ACMD23 pre-erase, a faster copy for one-sector callers.
Tooling notes: `sd-bench` prints the bus width, the probe result and a clock sweep (`clock_sweep()`); its raw experiments need `apx_sd_sync()` after the driver's reference read (fixed). A launch followed by an immediate `debug info` often fails once with "APXD response length mismatch" (reset that device with pyusb and ask again; never relaunch over a
running image). When an image falls back to stock firmware the desktop auto-mounts the card read-write and the stock firmware may create its own folders on it.

**Could the Silvergear run four bits? Probably not (analysis 2026-11-10, no hardware probe run).** (1) Stock: the Silvergear stock image has the same unreachable branch (controller init `0x02074bd4`, wide routing at `0x02074c50`, width routine `0x02074ee0`), its three card-init callers (`0x0200276c`, `0x0200f8dc`, `0x0200f94c`) each load the width argument as 0, and stock sends ACMD6 with argument 0: no evidence that four bits ever ran on this board. (2) Pads: the branch routes PA0..PA3 + PE10/PE11, i.e. six pads = CLK, CMD, DAT0 (PA0..PA2, used by one-bit mode) and three more that can only be DAT1..DAT3. Stock uses no other SD pad group. On the Silvergear PA3, PE10 and PE11 are the printer's motor phases (`02052060`: PD0, PE10, PA3, PE11) and PE10/PE11 are also the two cameras' power-down controls, both confirmed on hardware; one net cannot be a stepper-driver or camera-power-down input and an SD data line. (3) The board photo (`printer-cam-pcb-photos/pcb-back.jpg`, socket crop) shows several empty resistor/pull-up footprints next to the socket pins, consistent with DAT1..DAT3 not being fitted, but it does not prove it. Routing those pads to the SD controller would also pull up or drive the motor and camera nets. **Probe on the Silvergear (2026-11-10, RAM-only `sd-bench` built with `-DAPX_SD_BUS_WIDTH_MAX=4`, Samsung FD4Q9, 3 runs):** `wide probe 4` (APX_SD_WIDE_READ), detail `0xfe` (APX_SD_CARD_ERROR): the card accepted ACMD6 and the controller switched to four lines, then the four-bit SD-status read came back with a receive/CRC error. DAT1..DAT3 do not deliver data on this board, as predicted; the driver fell back to one bit and the card worked normally (5.5 MB/s, identical to the reference). Decision: `APX_SD_BUS_WIDTH_MAX 1` stays. (A meter check of socket pins 8, 1 and 2 = DAT1, DAT2, DAT3 would still show whether they reach any SoC pin.)

**The fallback was not robust at first, and what hardware showed.** The first version restarted the init at once. On the Silvergear that init failed (`-5`, not ready), and a fresh `apx_sd_init()` a second later worked; with a diagnostic build the card was seen to answer neither CMD8 nor CMD0 for a while (command timeouts) after the aborted four-bit read, and for a given build the recovery was deterministic but timing dependent (9.5 s and four failed attempts in one build, success on the second attempt in another). The restart now waits (`APX_SD_WIDE_SETTLE_TICKS`, 500 ms) and retries with doubling waits (0.5, 1, 2, 4 s); on the Silvergear this recovers in one 531 ms init, 3 of 3 runs. The model gained a card that is deaf for a period after a failed four-bit read (and one that never comes back: init then fails with a timeout instead of hanging); mutants (no retry, no settle, no doubling, no restart) are killed. A failed four-bit probe therefore costs about half a second once per card, and a failed `apx_sd_init()` records how far four bits got (`bus_probe`) even when init fails. Not re-run on the Action since this change (its card was in the Silvergear); that path (probe passes) is unchanged.

**Hot removal at four bits (Action Vision, Samsung FD4Q9, `examples/sd-card-test` mode 1, 2026-11-10).** Run 1 (420 s): the owner pulled and reinserted the card 50 times in all phases (idle reads, CMD18 read stream, CMD25/CMD24 write stream into the gap): 50 of 50 recoveries, no hang, longest call 520 ms (a write-phase pull, the driver's wait for the card), 7,558 completed write generations and 511,283 reads, every pull detected at once (read stream error -2 at CMD18 after 11 ms, write phases error -1 at CMD25/CMD24 after about 515 ms, idle reads error -1 at CMD17 after 111 ms), recovery 1.5-4.9 s (mostly the time to reinsert; init is retried every 333 ms), all 62 gap sectors one complete generation (0 torn), gap restored and verified. Run 2 (240 s, the test now prints the bus width and probe result at every recovery): 14 of 14 recoveries, **all back at four bits** (`bus 4 probe 1`), 0 on one line, 5,872 write generations, 253,748 reads, longest call 520 ms, 0 torn sectors, gap restored; one recovery took 11.5 s (reinsertion). So a card that is pulled and reinserted is re-probed and comes back on four lines. Not tested: the USB-disk side of hot removal at four bits (`tools/test_usb_msc_hotpull.py`), other cards.

