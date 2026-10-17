# JT5295 SD writes and formatting

The later [progressive image work](progressive-uart.md) adds bounded sector
retries for intermittent controller failures under sustained random paging.
Each replay follows card/controller reinitialization and CID/capacity checks;
the clock and transfer counters are preserved. Two retries are allowed before
the operation fails. SD info reports `retries`, `last_failure_command` and
`last_failure_status`. The original acceptance below predates this addition.

The SDK now writes sectors and files, formats FAT12/16, FAT32 and exFAT, and
provides writable picolibc FILE streams. The SD controller and DMA execute SDK
code; no vendor SD routines are called. The user explicitly authorized
destructive tests on the disposable inserted card. SPI is outside the write
path. This extends the [initial read-only milestone](sd.md).

## Controller evidence

Raw JT5295 instructions take precedence over Ghidra's generic OpenRISC
delay-slot decompilation. `sd-write-vendor-raw.disasm` preserves the relevant
instructions. `AnnotateSDWrite.java` adds reproducible Ghidra labels and
comments; its execution output is `sd-write-ghidra-annotations.txt`.

| Vendor address | Evidence | SDK behavior |
| --- | --- | --- |
| `02050df0` | DMA address/length at SPR 9890/9894, EVENT 9884 bits 4 and 5 | Transmit arm; receive uses bit 4 alone |
| `02050e28` | CONTROL 9880 bit 1 clocks the card; wait for bit 20 | Wait for DAT0 programming completion with a deadline |
| `02050f48` | Write status rejects CONTROL bit 16 | Reject failed transmit after DATA_DONE bit 15 |
| `020321bc` | CMD13 until `(R1 & 0x1f00) == 0x900` | Verify READY_FOR_DATA and transfer state |
| `020322dc` | Write command precedes transmit arm; SDSC byte vs SDHC sector address | CMD24 followed by single-sector DMA |
| `02022c98` | SPR 181c line count then 180c source address | Clean the transmit bounce buffer before DMA |

The first physical 512-byte write completed in about 1.98 ms, read back exactly,
and was restored and verified (`sd-write-first-block.json`). Further physical
tests wrote seven sectors and restored their original contents before formatting.
This verifies the transmit sequence on this board/card. The meaning of DAT0
clocking follows the stock wait routine; an external logic-analyzer trace is
not available.

The [SD Association physical-layer specification](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver6.00.pdf)
defines CSD permanent/temporary write protection at bits 13/12, R1
WP_VIOLATION at bit 26, CMD24 addressing and the ready/transfer-state fields.
The SDK respects those protection signals. A mechanical socket write-protect
sensor has not been mapped. Transfers remain one-bit at nominal 24 MHz with
one CMD24 per sector; CMD25, four-bit operation and TRIM are deferred.

The driver owns an aligned 512-byte DDR bounce buffer, accepts unaligned DDR
caller buffers, cleans before transmit and invalidates before/after receive.
Ranges, count overflow and card capacity are checked before commands. Each
write waits for data completion and programming completion before returning.
An error stops DMA and invalidates the controller; no automatic write retry is
performed. USB, buttons and maintenance continue during bounded waits.

## Filesystem lifecycle and libc

`disk_write` maps to native CMD24; `CTRL_SYNC` waits for physical card readiness.
The FatFs [disk_write contract](https://elm-chan.org/fsw/ff/doc/dwrite.html),
[format parameters](https://elm-chan.org/fsw/ff/doc/mkfs.html) and
[sync semantics](https://elm-chan.org/fsw/ff/doc/sync.html) informed the port.
`apx_storage_sync` flushes the FatFs window and FSInfo as well as the card.
`apx_storage_unmount` syncs a healthy volume before detaching it. A controller
fault invalidates the volume; old handles must be discarded before remounting.
An ordinary unmount/remount retains controller counters and card initialization.

Sync, unmount and format reject any open file or directory with `FR_LOCKED`.
The small `third_party/fatfs/appotech-port.patch` exposes the actual FatFs
lock table and sync helper, tracks FAT12/16 root-directory iterators omitted
from that table, and services USB/buttons/maintenance during CPU-only exFAT
upcase-table generation. FatFs copyright/license text and CRLF source format
are retained. Its existing configuration remains: R0.16, read/write, exFAT,
mkfs, labels, UTF-8 heap-backed LFN, all OEM codepages, relative paths,
fastseek and 30 object locks. Initial OEM codepage is 437. RTC timestamps are
fixed at 2026-01-01 pending RTC support.

The supplied `ff.c` baseline SHA-256 is
`dbeae3d7d22dbc070983e31af897fa5e43d29c726a0a240fe9589ffe2ec0ef1e`.
To reapply the port to that original file, run
`patch --binary --batch -p1 < appotech-port.patch` in `third_party/fatfs`.
The patch also retains CRLF; reverse/apply roundtrip verification reproduces
the current source byte-for-byte (`sd-write-fatfs-patch-verification.json`).

`apx_storage_format` allocates 4 KiB of work space before changing the card,
syncs/unmounts, calls `f_mkfs` and remounts on success. Format defaults to an MBR
partition and two FAT copies; `FM_SFD` selects a superfloppy. Allocation-unit
parameters are checked before changes. Formatting replaces filesystem metadata
and does not securely erase old data. FAT12/16 is the host default for stock
compatibility; the inserted card receives FAT16.

`apx_fopen` supports read, write, append and update modes, binary markers and
exclusive `wx`. Its picolibc cookie writes synchronize FatFs data and metadata,
so `fflush` commits them. Append writes seek to EOF even after a seek. Seek,
short-write, close and FatFs errors propagate through stdio/errno. APXD recovery
syncs and unmounts before its ACK; open handles reject recovery. Direct C callers
close handles and successfully unmount before invoking `apx_bootrom()`.

APXD STORAGE commands 6 through 14 add raw/file writes, mkdir, unlink, rename,
labels, formatting, unmount and sync. The v1 header and 4 KiB payload limit
remain unchanged. Raw writes reject a mounted volume; the explicit host raw
command unmounts first. File uploads default to create-new, with an explicit
overwrite option. Each request closes its file before ACK. Offset/length/path
validation prevents malformed writes and giant sparse allocations. The
[SDK README](../README.md) records exact frames and CLI examples.

## Verification and limits

`sd-write-live-validation.json` records the final GPIO build's destructive
physical suite: FAT16, FAT32, exFAT and FAT16 again; Unicode/emoji filenames;
labels; directories; rename/unlink; sector-crossing patches; sizes
0/1/63/64/65/511/512/513/4095/4096/4097/65536; seven-sector raw write/readback
and restoration; mounted raw-write rejection; remount persistence; and 60
seconds of sustained file writes/readback. It completed 865 requests and
verified 1,771,520 bytes during the sustained phase, with zero SD errors or
button ADC timeouts. Formatting took about 3.7 s for FAT16, 15.0 s for FAT32
and 5.0 s for exFAT. Host format timeouts are extended to two minutes.

`sd-write-native-stdio.json` records the optional `sd-demo` C mailbox test:
fwrite/fflush, update seek/readback, append after seek, fclose, and rejection
of format/recovery with an open file or root DIR. All checks passed and the
host independently verified the 1,029-byte file. That report uses the build
before the exFAT cooperative hook was added; stdio code was unchanged by the
hook. The mailbox is inert until the host explicitly requests this destructive
test. No default example writes or formats at startup.

ASan/UBSan models cover SDSC and SDHC/SDXC addressing, unaligned buffers,
capacity bounds including the last 32-bit sector, DMA cache/order guards,
transmit errors, write protection, removal, timeouts and reinitialization.
Real FatFs on RAM disks covers FAT12/16/32/exFAT, the SDK lifecycle, writable
stdio, root/file locks, huge-offset rejection, create/replace validation and
write-protected behavior. Display/GPIO models and the host suite also pass.
Physical card coverage is the inserted 2 GB SDSC card; SDHC/SDXC and FAT12
physical validation need other cards. There is no power-loss atomicity claim.

During early cross-example testing, BootROM ACK, loader INQUIRY and RAM reads
passed, but subsequent stock firmware USB enumeration failed with Linux -71
and required a physical power-cycle. The final same-image recovery report
`sd-write-recovery-cycles.json` and acceptance record distinguish that earlier
event from the final recovery checks. The cause of the earlier enumeration
failure has not been established.

See `sd-write-acceptance.json` for final build hashes, linked/RAM sizes, model
and host checks, recovery cycles, full SPI comparison and the state left on
the device. The final card is partitioned FAT16, labeled `APX SDK`, with test
files; the stock firmware may recreate its own empty directories/list file
after recovery.
