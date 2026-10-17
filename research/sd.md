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
| `02050d58` | 9880 bit 2 | One/four-bit controller selection; SDK retains one bit |
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

Initialization uses the stock divider 359 (nominal 200 kHz); normal reads use
divider 2 (nominal 24 MHz). Clock 9888 reads back zero, so it is treated as
write-only. Clock frequency follows the vendor source constant and has not
been measured electrically. Four-bit operation and high-speed switching are
deferred. Card detection is command-based; close handles and remount after
removal/read failure. There is no hotplug GPIO driver yet.

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
card has physical validation. MMC, SDUC, four-bit transfer, writes and format
execution on a real card are not implemented.

Final live stress/recovery/preservation results are in `sd-acceptance.json`.
Card preservation claims cover captured sectors and file hashes; they are
not a full 2 GB card hash. SPI preservation uses a complete 2 MiB comparison.
