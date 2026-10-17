# JT5295 SDK

Bare-metal C SDK for the Action Vision/AppoTech JT5295 camera, launched through
USB or a board-specific SPI image.
The native profile uses little-endian code/data, calls linking to PC+4, inert
branch slots and software floating point. Multiply and divide run on the chip's `l.mul`/`l.div`/`l.divu`
through `src/hwmuldiv.S` (the compiler profile is still `-mappotech` software mul/div; `make HW_MULDIV=0` links libgcc's
routines instead, `examples/muldiv-test` checks 700 vectors and times both). GCC 16.2.1,
binutils 2.47 and picolibc 1.8.12 are built from pinned sources. The polling USB
driver accesses the controller directly; BootROM USB functions are only used
by the host bootstrap. Recovery deliberately jumps back into BootROM.

## Build

The compiler is built from two upstream trees at the commits pinned in
[`toolchain/sources.lock.json`](toolchain/sources.lock.json), which also holds the patch hashes. Check them out
on local branches named `gcc16-openrisc-appotech` and `binutils-openrisc-appotech` (the builder insists on
those names), in a `compilers/` directory next to this repository, or anywhere with
`export APPOTECH_COMPILERS=/path/containing/both`:

```sh
mkdir compilers && cd compilers
git clone https://github.com/gnutools/gcc
git -C gcc checkout -b gcc16-openrisc-appotech 71b50793e1e2b12b36baff23c1211e43b8c86ecd
git clone https://github.com/gnutools/binutils-gdb
git -C binutils-gdb checkout -b binutils-openrisc-appotech 6ce87bbc521cf46eaee9a1f7ef61cee2cdfb3e32
```

Any clone that contains those commits will do. All builds and installed tools live outside the compiler source trees.

Host dependencies: a native C/C++ build toolchain, GCC build prerequisites
(GMP/MPFR/MPC, flex and bison), Meson, Ninja, Python, PyUSB and pyelftools.
Install [apxtool](https://github.com/matu6968/apxtool) with its `usb` extra (`pip install ./apxtool[usb]`), or clone it next
to this repository and set `PYTHONPATH=../apxtool/src`, as the examples below do. They assume the two repositories sit side by
side as `appotech-sdk/` and `apxtool/`.

```sh
python3 toolchain/build.py binutils
python3 toolchain/build.py gcc
python3 toolchain/build.py picolibc
python3 toolchain/build.py libgcc
make
python3 tools/test_toolchain.py
```

The flash and recovery payload is built separately: `make -C boot` produces
`build/flasher/flasher.bin`. It is one position-independent image (the build links it at
the SRAM and the DDR base and fails unless the bytes match) that `apxtool flash` and
`apxtool rom-rescue` upload to whichever pool the loader has; see
the [apxtool README](https://github.com/matu6968/apxtool#flashing).
`python3 tools/test_flasher_model.py` runs its logic against a host model of the SPI
controller, NOR chip, PM bus and cache.

The builder verifies commits and patch hashes, applies unapplied patches,
and accepts already patched trees. It never switches branches or commits.
To export intentional compiler changes, run `toolchain/export_patches.py`.
Picolibc is cloned at tag `1.8.12`, commit
`2ae376c6cdf4fef90ca2388ecf7a07457fa63cff`, then patched for the native endian
profile and no-delay `setjmp`. Its Meson configuration uses one architecture,
single-threaded operation, no TLS, no semihosting or libc startup, and integer
formatting with 64-bit integer support. Configuration follows the pinned
[build documentation](https://github.com/picolibc/picolibc/blob/1.8.12/doc/build.md).

Build outputs include `libappotech.a`, `libfatfs.a`, ELF, map, disassembly, segment binaries
and a manifest containing hashes, reservations, entry point and toolchain
provenance. Example configuration:

```sh
make OPT=-O2 HEAP_SIZE=32768 USB_VID=0x1908 USB_PID=0x3319
make LTO=1                # enable GCC link-time optimization
make FORCE_FULL_SPEED=1  # diagnostic build: disable high-speed negotiation
make                    # restore the default high-speed build
```

`EXAMPLE` selects which `examples/<name>/main.c` is linked, and names its ELF
and manifest under `build/`. It only affects that `make` invocation; pass the
same example's manifest to the launch command:

```sh
make LTO=1 EXAMPLE=camera-demo
apxtool --device /dev/sg0 sdk launch build/camera-demo.manifest.json --reclaim-jpeg-ram
```

The optional LVGL 10.0.0 port can be built and launched with:

```sh
make EXAMPLE=lvgl-demo
apxtool --device /dev/sg0 sdk launch build/lvgl-demo.manifest.json
```

By default the demo calls `lv_demos_create()` to run LVGL's benchmark demo with the
software renderer and a full-screen RGB565 buffer in the extra DDR bank.
Full-frame refreshes use the LCD DMA path; UP/DOWN move keypad focus, LEFT/RIGHT
are passed to the focused widget, SHUTTER selects, and POWER sends Escape.
The benchmark and widgets demo assets, LVGL code, and renderer heap use the
preflighted extra DDR and codec-RAM reservations. Other examples do not compile
or link LVGL. As with JPEG demos, if the launch tool reports codec RAM is
occupied, back it up before using the launcher's `--reclaim-jpeg-ram` option.
Build with `make EXAMPLE=lvgl-demo LVGL_DEMO=widgets` to run the interactive
widgets demo instead; it is useful for checking keypad navigation as well as
rendering.

LTO is enabled by default. Its setting is included in the build ID and manifest;
switching modes automatically rebuilds objects and uses GCC's LTO-aware archiver.
Objects whose code or tables are assigned to dedicated RAM regions by input-file
name remain non-LTO so those linker placements are preserved; the rest of the
SDK and application are built with LTO.

## Launch and debug

Install `udev/99-appotech-sdk.rules` and reload/trigger udev as described in
the commands below. Launch from the camera's current SCSI generic path:

```sh
sudo cp udev/99-appotech-sdk.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
sudo udevadm trigger --subsystem-match=scsi_generic
```

```sh
apxtool --device /dev/sg0 sdk launch build/camera-demo.manifest.json --reclaim-jpeg-ram
apxtool debug --port 7-1 info
apxtool debug --port 7-1 logs
apxtool debug --port 7-1 read 0x02000000 64 --width 4
apxtool debug --port 7-1 spr-read 0x9804
apxtool debug --port 7-1 bootrom
```

For another build, replace `camera-demo` with the value passed to `EXAMPLE`;
for instance, `EXAMPLE=usb-debug` uses `build/usb-debug.manifest.json`.

Launch checks that all manifest RAM reservations are zero, verifies every bounded
upload by readback, then performs a non-returning takeover. Only an APXD
handshake with the expected build ID counts as launch success. Host cleanup
never invokes the old SCSI callback after takeover. Recovery must acknowledge
the request before disconnecting; the host tracks the physical port and
verifies loader INQUIRY and a RAM read. Do not launch concurrently with other
camera tools.
After verified recovery, `debug bootrom` releases the SDK's RAM reservations
to their preflighted zero state. This enables subsequent launches without
touching the active SDK through a vendor callback.

## Runtime and protocol

`include/appotech/` exposes SPR and typed MMIO operations, cooperative ticks
and deadlines, USB polling/buffers, diagnostic logs and BootROM return.
The linker reserves `0x02180000..0x0219ffff` and a 4 KiB downward-growing stack
at `0x02200000..0x02200fff`. Display builds also reserve
`0x02210000..0x0221ffff` for the complete NV21 DMA conversion workspace;
USB-only builds retain the original two reservations. The default heap is 16 KiB. SDK `sbrk` returns
`ENOMEM` on exhaustion; picolibc owns allocation and memory/string functions.
Stdout/stderr use a bounded 4 KiB diagnostic ring retrieved through APXD LOG.
The full-frame display demo places its RGB565 framebuffer in the existing
`0x02220000..0x022dffff` extra DDR bank and reserves that entire bank in its
manifest. FatFs builds use the same bank for Unicode tables. `.large_buffer`
allocations follow those tables when both are linked; the linker checks capacity.
The LVGL demo places its renderer heap and RGB565 framebuffer in this same
preflighted extra DDR bank. Its renderer code and demo image assets use the
codec-RAM reservation also used by JPEG workspaces.
All extra DDR is zero-preflighted, protected and released through the same
verified handoff as other SDK reservations.
File-backed read/write/append stdio is available through `apx_fopen`; see SD below. See picolibc's
[OS integration documentation](https://github.com/picolibc/picolibc/blob/1.8.12/doc/os.md).

The host initializes ELF load segments at their VMA; startup handles `.data`
and clears `.bss`. RAM initialization is inherited from the loader; SDK startup
now selects the accepted approximately **144 MHz** PLL1 pair before peripheral
initialization. `make ... CPU_MHZ=96` retains the 96 MHz loader profile. Both
options are hash-bound in the manifest. Recovery restores the loader pair after
peripheral/DMA shutdown. [Clock profiles and timing evidence](research/clock.md)
describe the aligned transition and clock-dependent audio rate settings.
Ticks accumulate the 14-bit reference counter; call the polling loop
at intervals shorter than 0.5 seconds to avoid losing a wrap. Deadline comparisons support
intervals below 2^31 ticks. The reference frequency is inferred as 32768 Hz. `apx_delay_ms` is a busy
initialization delay with a maximum supported argument of 130000 ms; application
waits should use deadlines while continuing USB polling.

APXD v1 uses the 24-byte little-endian header `<4sBBBBIIII>`:

| Offset | Field |
| --- | --- |
| 0 | `APXD` magic |
| 4 | Version 1 |
| 5 | INFO=0, READ=1, WRITE=2, BOOTROM=3, LOG=4, STORAGE=5, GPIO=6, SYSTEM=7, JPEG=8, AUDIO=9, MICROPHONE=10, CAMERA=11, SPI=12, BATTERY=13, UART=14 |
| 6 | MEMORY=0, MMIO=1, SPR=2 |
| 7 | Width 1/2/4; bit 7 explicitly overrides SDK RAM protection |
| 8 | Sequence |
| 12 | Address or SPR number |
| 16 | Payload/request length, at most 4096 |
| 20 | Status: OK=0, INVALID=1, RANGE=2, UNSUPPORTED=3, PROTECTED=4, IO=5; FatFs errors=0x100+FRESULT |

Requests carry payload for WRITE and service operations STORAGE through CAMERA.
Response length is its actual payload
size. SPR requests address one 32-bit register by number; memory/MMIO addresses
must be aligned to their width. Protected writes require host `--raw`. Bad
magic/oversized/incomplete frames are discarded; well-framed invalid commands
return status. The interface is vendor class with OUT `0x01` and IN `0x81`,
512-byte HS or 64-byte FS packets, qualifier and other-speed descriptors.
Prototype IDs default to `1908:3319`; identification uses the handshake because
BootROM uses the same IDs.

## Evidence and scope

See [`research/evidence.md`](research/evidence.md), the aggregate
`research/acceptance.json`, and the JSON test records.
`tools/test_usb.py` checks live descriptors, boundaries, protection, halt
recovery and sustained transfers in preflighted scratch RAM. Changing RAM image layouts
back-to-back in a warm loader is unverified: use a fresh stock-firmware entry or a
physical reset when changing firmware builds; the ten tested SDK recovery cycles use
the same image.

The USB path is polling and PIO; unused vendor DMA and external interrupts are
disabled. Unknown custom opcodes, general exception handling, arbitrary bad
address fault recovery, general DMA services and GDB are not established. Display
DMA is implemented through the independent compositor/LCD path. Undocumented register
writes may need a physical reset. Generic OpenRISC code generation and big-endian
ELF remain available; the installed libc/libgcc configuration is exclusively
JT5295. PIC is rejected for `-mappotech`.

The independent display driver is available; see the usage below and
[`research/display.md`](research/display.md) for register/panel evidence.
SD/FatFs, GPIO, six board buttons, the blue status LED, watchdog/power and
hardware JPEG are available below, with USB debugging retained throughout.
Speaker output, microphone capture, battery indication and independent SPI NOR
are also available. Continuous MJPEG and further ISP controls follow later.
Cold boot precedes application deployment. This milestone
does not change the SPI image.

## Display

The board has an ILI9225G panel with native 176×220 GRAM, mounted in landscape.
The SDK defaults to upright **220×176 RGB565**. The display driver resets the
panel, configures its parallel bus and applies the stock power/gamma sequence
directly. Commands use bounded PIO; pixel data can use PIO or polling compositor/LCD
DMA. It calls no vendor routines, enables no display interrupts and allocates no heap.
USB debugging remains usable
during initialization and redraws. BootROM recovery shuts the display down.

```sh
make EXAMPLE=display-demo
apxtool --device /dev/sg0 sdk launch build/display-demo.manifest.json
PYTHONPATH=../apxtool/src python3 tools/display_demo.py --pattern gradient
PYTHONPATH=../apxtool/src python3 tools/display_demo.py --pattern bars
PYTHONPATH=../apxtool/src python3 tools/display_demo.py --test --output research/display-pattern-tests.json
PYTHONPATH=../apxtool/src python3 tools/display_demo.py --mode pio --pattern bars
PYTHONPATH=../apxtool/src python3 tools/display_demo.py --mode dma --benchmark
PYTHONPATH=../apxtool/src python3 tools/display_demo.py --native-test
python3 tools/test_display_model.py
```

The example starts with labeled color bars sent through RGB565 DMA, and provides solid colors, a gradient,
a checkerboard and a partial rectangle test. Its host tool identifies the
mailbox from the ELF and checks the SDK build ID. Only that example's command
fields and its host-prepared native frame are written using APXD's explicit raw override; the library itself has
no host protocol dependency. A full example framebuffer consumes 77,440 bytes
inside the existing SDK reservation. The default heap remains 16 KiB.
The separate 64 KiB DMA reservation contains a 63,360-byte conversion workspace;
launch preflights it, APXD protects it, and acknowledged recovery releases it.

Applications can use `include/appotech/display.h` directly:

```c
apx_usb_init();
apx_display_init();                    /* asynchronous panel initialization */
unsigned submitted = 0;
for (;;) {
    apx_debug_poll();
    apx_display_poll();
    if (!submitted && apx_display_status() == APX_DISPLAY_READY) {
        submitted = apx_display_fill(0, 0, apx_display_width(),
                                    apx_display_height(), APX_RGB565(0, 80, 160)) == 0;
    }
}
```

`apx_display_blit(x,y,w,h,pixels,stride_pixels)` queues a strided RGB565
rectangle; keep its source valid and unchanged until READY or ERROR.
`apx_display_fill` requires no source buffer. Both return `-1` for invalid
bounds, alignment, stride, arithmetic overflow or a busy/uninitialized panel.
Poll at least every few milliseconds while busy. A failed bus byte is bounded
to approximately 5 ms and leaves the engine/backlight off with ERROR status;
call `apx_display_init` to reset and retry. Calling init also aborts a pending
redraw. Backlight control is exposed through `apx_display_backlight`.

`apx_display_blit_dma(x,y,w,h,pixels)` accepts a contiguous RGB565 DDR source
with even width and height. It converts two rows per poll to full-range BT.601
NV21, preserves the source, and sends the complete frame using one DMA transfer.
Chroma is averaged over 2×2 pixels; use the PIO blit for exact RGB565 pixels,
odd rectangles or strided sources. The converter uses the static DMA workspace.
For an existing full RGB565 framebuffer, replace its queued PIO blit with
`apx_display_blit_dma(0,0,APX_DISPLAY_WIDTH,APX_DISPLAY_HEIGHT,framebuffer)`
and continue polling USB and the display until READY.

`apx_display_blit_nv21(x,y,w,h,luma,chroma,pitch_bytes)` sends a native frame
without CPU conversion. Both DDR planes and the byte pitch must be aligned to
32 bytes; dimensions must be even. The Y plane spans `pitch*h` bytes and the
interleaved **V,U** plane spans `pitch*h/2` bytes. For 220×176, use pitch 224
and 59,136 bytes total. Keep both planes unchanged until READY or ERROR.
Each DMA frame has a one-second deadline and completes only when both the
compositor and LCD report completion. Shutdown disables both engines before
releasing source pointers. `apx_display_last_transfer_ticks()` includes CPU
conversion and window setup, with an inferred 32,768 Hz tick rate.

`apx_display_set_rotation` accepts 0/90/180/270 while idle or off. Query logical
dimensions with `apx_display_width/height` after changing rotation. Each queued
rectangle programs panel traversal, so rotations do not require software pixel
conversion. The board's default landscape mode is optically verified; other
rotations are covered by the panel model tests.

Full-frame PIO takes roughly 266 ms in the driver; converted RGB565 DMA takes
roughly 66 ms, and native NV21 DMA roughly 17 ms. Example rendering and host
polling add time. Tear synchronization and panel power management remain deferred.
Build `make` to select the USB-only example again; it reserves no display workspace.
See [`research/display-dma.md`](research/display-dma.md) for the register sequence,
corrected early interpretations, benchmarks, exact build sizes and acceptance records.

## SD and FatFs

`make EXAMPLE=sd-demo` builds independent native SD receive/transmit DMA plus your FatFs
R0.16 module. The backend reads SDSC and SDHC/SDXC cards in one-bit mode,
using nominal 200 kHz identification and 24 MHz transfers. The inserted SDSC
card is physically verified with FAT16, FAT32 and exFAT; other card capacities have model
coverage. USB debugging and the existing display driver remain available.
The example shows dark gray before mounting and blue after mounting, and
checks a read-only picolibc FILE stream on the first regular file.

```sh
make EXAMPLE=sd-demo
apxtool --device /dev/sg0 sdk launch build/sd-demo.manifest.json
apxtool debug --port 7-1 sd volume
apxtool debug --port 7-1 sd info
apxtool debug --port 7-1 sd list 0:/
apxtool debug --port 7-1 sd list 0:/PHOTO
apxtool debug --port 7-1 sd get 0:/PHOTO/PHO00000.JPG photo.jpg
apxtool debug --port 7-1 sd read-sector 0 --count 8 --output sectors.bin
apxtool debug --port 7-1 sd put notes.txt 0:/notes.txt
apxtool debug --port 7-1 sd put notes.txt 0:/notes.txt --overwrite
apxtool debug --port 7-1 sd mkdir 0:/Documents
apxtool debug --port 7-1 sd rename 0:/notes.txt 0:/Documents/notes.txt
apxtool debug --port 7-1 sd label "APX SDK"
apxtool debug --port 7-1 sd sync
apxtool debug --port 7-1 sd codepage 932
apxtool debug --port 7-1 bootrom
python3 tools/test_sd_model.py
python3 tools/test_fatfs_model.py
```

Host downloads create new files and refuse overwrites. `sd volume` mounts on
demand; `sd list` also ensures a mount. `sd info` reports initialization,
capacity, reads/writes/errors and write protection. Uploads create new files;
`--overwrite` explicitly replaces an existing file. `sd unlink PATH` removes a
file or empty directory. `sd format --filesystem fat` creates a new volume and
destroys existing card contents; `fat32`, `exfat` and `auto` are also available.
The default is a partitioned FAT12/16 volume (`fat`), selected for stock firmware
compatibility on this 2 GB card. `--cluster-bytes` sets the allocation unit;
`--superfloppy` omits the partition table. Formatting does not erase every data
sector. A failed transfer invalidates SD state; close/discard application handles
and mount again after reinsertion.

The requested exFAT, mkfs, label, LFN, relative-path, fastseek and file-locking
options remain enabled. All OEM codepages are compiled (`FF_CODE_PAGE=0`),
with 437 as the initial short-name codepage and `f_setcp()` for selecting another.
Paths use UTF-8; LFN buffers use picolibc malloc/free to avoid consuming the
4 KiB stack. Allocation can fail normally if the 16 KiB heap is exhausted. RTC
is not implemented yet, so timestamps are fixed. CSD permanent/temporary write
protection and the card's write-protection response are respected. The socket
has no verified mechanical write-protect sensor. The backend uses synchronous
single-block CMD24 writes and waits for card programming to finish; multi-block
transfers, four-bit mode and TRIM remain deferred.

Applications include `appotech/sd.h` for sectors and `appotech/storage.h` for
FatFs integration; add `-Ithird_party/fatfs` and link `libappotech.a` plus
`libfatfs.a` in a linker group. Use a single cooperative task. Waits service
USB polling and maintenance; they do not recursively dispatch debug commands.
Mount after the launch handshake or before connecting SDK USB, to avoid
holding the host's first INFO request during card initialization.
Use `apx_storage_sync`, `apx_storage_unmount` and `apx_storage_format` for volume
lifecycle operations. They reject open files and directories with `FR_LOCKED`.
Close all handles before unmounting, formatting or removing the card. APXD
BootROM recovery syncs/unmounts before acknowledging; a direct C call to
`apx_bootrom()` requires the application to close handles and successfully call
`apx_storage_unmount()` first. Controller failures invalidate cached state and
do not trigger automatic write retries.

```c
FRESULT result = apx_storage_mount();
if (result == FR_OK) {
    FILE *file = apx_fopen("0:/README.txt", "w");
    if (file) {
        if (fputs("JT5295 SDK\n", file) == EOF || fflush(file) != 0) {
            /* Handle a full card, removal or a controller error. */
        }
        if (fclose(file) != 0) { /* Handle the close error. */ }
    }
}
```

All-codepage tables need 483036 bytes of read-only data. Storage builds reserve
`0x02220000..0x022dffff` separately, with the same zero preflight, segment hash,
write protection and acknowledged recovery rules as other SDK RAM. The SD
demo's current footprint is recorded in write acceptance,
including its 16 KiB heap, plus the existing 4 KiB stack and 64 KiB display
workspace. Applications do not need a framebuffer or heap allocation for
sector DMA: the driver owns a 512-byte aligned
bounce buffer. Rebuilds retain source/configuration hashes in the manifest.
Formatting allocates a 4 KiB workspace from the same heap. The small FatFs port
patch preserves object locks across lifecycle operations, tracks root-directory
iterators and services the cooperative loop during exFAT table generation;
see `third_party/fatfs/appotech-port.patch`.

The APXD 24-byte header is unchanged. STORAGE operation 5 uses MEMORY/width 1,
address as subcommand and length as request-payload bytes. All integers are
little-endian and responses remain at most 4096 bytes:

| Subcommand | Request | Response |
| --- | --- | --- |
| 0 INFO | Empty | Card/status JSON |
| 1 BLOCK | u32 sector, u32 count(1..8) | count×512 bytes |
| 2 VOLUME | Empty | u32 FS kind, u32 cluster sectors, u32 serial, UTF-8 label+NUL |
| 3 LIST | u32 index, UTF-8 path+NUL | u64 size, u32 attributes/date/time, name+NUL; empty at end |
| 4 FILE | u64 offset, u32 count(0..4096), UTF-8 path+NUL | File bytes; empty at EOF |
| 5 CODEPAGE | u32 OEM codepage | Empty |
| 6 BLOCK_WRITE | u32 sector, 1..7 complete sectors | Empty; rejected while mounted |
| 7 FILE_WRITE | u64 offset, u32 mode, u32 path bytes, path+NUL, data | u32 bytes written |
| 8 MKDIR | Path+NUL | Empty |
| 9 UNLINK | Path+NUL | Empty |
| 10 RENAME | u32 old-path bytes, old-path+NUL, new-path+NUL | Empty |
| 11 LABEL | `0:LABEL`+NUL | Empty |
| 12 FORMAT | u32 FatFs format flags, u32 cluster bytes (0=default) | Empty |
| 13 UNMOUNT | Empty | Empty |
| 14 SYNC | Empty | Empty |

FILE_WRITE modes are 0=existing, 1=create new, 2=replace; modes 1/2 require
offset zero. Existing-file offsets cannot exceed EOF through APXD. Path lengths
include the terminator; the complete request is limited to 4096 bytes. Every
file-write request closes and syncs before its response. FatFs errors use APXD
status `0x100 + FRESULT`. `sd write-sector SECTOR INPUT` explicitly unmounts
before bounded raw writes; use it only when editing filesystem sectors is intended.

`apx_fopen` accepts `r`/`w`/`a`, optional `b` and `+`, and `x` for exclusive `w`.
It returns a picolibc FILE usable with standard C I/O. `fflush` commits data and
metadata; append writes remain at EOF after a seek. Direct FatFs APIs provide
stat, directories, labels, relative paths and 64-bit exFAT file sizes.
See [write/format evidence](research/sd-write.md) and acceptance,
and the earlier [read-only SD evidence](research/sd.md)
for physical/model coverage, limitations and preservation checks.

## GPIO, buttons and LEDs

Build `gpio-demo` for six labeled screen tiles, debounced board inputs, blue
status LED control and the SD read/write/format and USB interfaces:

```sh
make EXAMPLE=gpio-demo
apxtool --device /dev/sg0 sdk launch build/gpio-demo.manifest.json
apxtool debug --port 7-1 gpio info
apxtool debug --port 7-1 gpio led on
apxtool debug --port 7-1 gpio watch --seconds 30
apxtool debug --port 7-1 gpio led off
apxtool debug --port 7-1 sd list 0:/
apxtool debug --port 7-1 bootrom
```

Unmount camera volumes before RAM takeover. The SDK does not power off on a
short Power press. Holding Power for two seconds in `gpio-demo` requests
graceful shutdown, which rejects open file/directory handles. Applications
choose their own button policy. Screen tiles are SHUTTER, DOWN,
UP on the top row and LEFT, RIGHT, POWER below, turning green while pressed.
The blue status LED uses PB5 forced-high/low output; display backlight uses
`apx_display_backlight`. The battery overlay uses a plus sign for external power; charge-active/full status remains unidentified.

`gpio.h` provides five banks, masked pin reads/writes, input/output direction,
pull-up/down, initial output level and forced-high/low/normal pad control.
These are cooperative RMW APIs: the caller must own the selected pins and
must not reconfigure pins in use by a peripheral. Output levels are applied
before drive enable. No GPIO interrupt API or generic alternate-function
selector is exposed yet. Display reset, RS and backlight share this library.

`buttons.h` exposes `apx_buttons_init`, `apx_buttons_poll`,
`apx_buttons_get_info`, `apx_buttons_next`, `apx_led_set/get` and shutdown.
Poll alongside USB/display at least every 5 ms. The board uses ADC channel 3
for five ladder buttons and PB0 for active-high Power. PB1 is also configured
as a floating input to match the stock board setup. Debounce requires 20 ms
of stable state; ADC conversions have bounded 5 ms deadlines. One ladder
button at a time is supported; ladder chords are electrically ambiguous.

Events contain u32 ticks, state, pressed and released masks. The queue holds
16 events, discards the oldest on overflow and counts drops. ADC timeouts,
raw ADC value and raw/debounced state are available in the status snapshot.
SD wait loops continue input sampling. The GPIO demo leaves queued events for
the host; applications may consume them directly. BootROM recovery stops ADC
sampling and restores the owned GPIO fields and clock bit.

GPIO operation 6 retains the APXD header, MEMORY space, width 1 and at most
4 KiB payload. Address selects the subcommand:

| Subcommand | Request | Response |
| --- | --- | --- |
| 0 STATUS | Empty | JSON: state, raw_state, adc, power, led, adc_timeouts, events_dropped, button names |
| 1 LED | u32 enabled, exactly 0 or 1 | Empty |
| 2 EVENTS | Empty | Up to 16 records: u32 ticks, state, pressed, released; queue is drained |

Bit order is SHUTTER=1, DOWN=2, UP=4, LEFT=8, RIGHT=16, POWER=32. The extension
returns UNSUPPORTED until the application initializes board inputs; USB-only
and display-only examples do not initialize them. See [GPIO evidence](research/gpio.md) and
acceptance for hardware/model results.

```sh
python3 tools/test_gpio_model.py
python3 tools/test_display_model.py
python3 tools/test_sd_model.py
python3 tools/test_fatfs_model.py
python3 tools/test_gpio_live.py --seconds 60 --output research/new-gpio-live.json
python3 tools/test_cycles.py --manifest build/gpio-demo.manifest.json --sd --gpio --count 10 --output research/new-gpio-cycles.json
```

## Watchdog, reboot and power-off

Every example starts the hardware watchdog in automatic mode, selector 5
(about 1.8 seconds on this board). `appotech/watchdog.h` exposes start, feed,
stop and status. SDK maintenance respects this policy: automatic mode feeds
in polling/wait loops; manual mode feeds only through `apx_watchdog_feed` or
the explicit host command. Manual mode can detect a missing application
heartbeat even while USB and peripheral polling continue. Disabling the
watchdog remains effective across those loops.

| Selector | Observed timeout on this camera |
| --- | --- |
| 4 | About 0.46 s |
| 5 | About 1.82 s |
| 6 | About 7.31 s |
| 7 | About 29.10 s |

Selectors 0–3 are accepted hardware encodings, inferred to decrease by factors
of four, and have not been physically timed. The watchdog uses an approximate
RC clock; feed comfortably before expiry. It is independent of the SDK tick
counter. Choose enough time for the longest operation in manual mode; SDK
waits keep USB responsive but do not extend a manual watchdog deadline.

```c
#include <appotech/watchdog.h>
#include <appotech/debug.h>
apx_watchdog_start(6, APX_WATCHDOG_MANUAL);
for (;;) {
    apx_debug_poll();
    /* Run the application's work and check its health. */
    apx_watchdog_feed();
}
```

`appotech/power.h` provides `apx_power_off()` and `apx_reboot()`. They sync and
unmount storage before shutdown, rejecting open files/directories with
`0x100 + FR_LOCKED`. A preparation failure returns an APXD status without
disconnecting USB; successful calls never return. The two-phase prepare/commit
API lets a host command be acknowledged before teardown. Power-off follows the
stock always-powered register-bank sequence; reboot uses a fast watchdog reset
and starts the stock SPI firmware. `apx_watchdog_reset()` is the low-level
unsynchronized reset for faults. BootROM return remains a separate API/command.
The ROM establishes its own watchdog policy after that handoff.

```sh
apxtool debug --port 7-1 system info
apxtool debug --port 7-1 system watchdog start --selector 6 --manual
apxtool debug --port 7-1 system watchdog feed
apxtool debug --port 7-1 system watchdog stop
apxtool debug --port 7-1 system watchdog start --selector 5
apxtool debug --port 7-1 system reboot
# After another SDK launch:
apxtool debug --port 7-1 system power-off
```

SYSTEM operation 7 uses MEMORY/width 1 and the unchanged APXD v1 header:

| Subcommand | Request | Response |
| --- | --- | --- |
| 0 INFO | Empty | Watchdog status and raw power-bank bytes as JSON |
| 1 WATCHDOG_START | u32 selector (0..7), u32 automatic (0/1) | Empty |
| 2 WATCHDOG_FEED | Empty | Empty; I/O error if disabled |
| 3 WATCHDOG_STOP | Empty | Empty |
| 4 REBOOT | Empty | Empty ACK before hardware reset |
| 5 POWER_OFF | Empty | Empty ACK before board shutdown |

The host prints acknowledgement; this alone does not establish reboot or
electrical power removal. Live test tools track the same physical port and
verify the new MSC identity/file contents. Power-off/Power-button restart has
been exercised on this USB-connected board. Charging/USB standby power can
remain; rail currents and battery-only standby have not been measured.
`gpio-demo` adds a two-second Power hold; a short press still displays its tile.

```sh
python3 tools/test_system_model.py
python3 tools/test_watchdog_live.py --elf build/gpio-demo.elf --seconds 60 --output research/new-watchdog-live.json
```

See [watchdog/power evidence](research/watchdog-power.md) and
acceptance for raw register sequences,
hardware timing, file durability, RAM sizes and SPI preservation.

## Hardware JPEG

`appotech/jpeg.h` exposes the independent polling JPEG engine; no vendor codec
functions are called. Baseline eight-bit, three-component, single-scan 4:2:0
and 4:2:2 files decode to full-range NV21. Sources up to 2048×1536 can be
scaled to a display-size output (at most 224×220). Progressive, 4:4:4,
grayscale and multiple scans return `APX_JPEG_UNSUPPORTED` before starting
DMA. No progressive hardware path has been verified.

The SD preview service automatically uses the bundled **stb_image 2.30 software
decoder** for progressive JPEG, baseline 4:4:4/grayscale JPEG and supported
non-JPEG images. Compatible baseline 4:2:0/4:2:2 files retain hardware decoding.
Eight-bit JPEG inputs up to 2048×1536 are supported, including quality-100
files; 12-bit and arithmetic JPEG are unsupported. PNG (including 16-bit
converted to 8-bit), BMP, TGA, GIF's first frame and binary PNM also work when
their decoded image fits the bounded workspace. Large non-JPEG images may
return a memory-range error. Alpha is discarded.

`appotech/image.h` exposes this software backend separately. It requires
caller-owned input, NV21 output and workspace; no unbounded libc allocation
is used. Large JPEG coefficient arrays are losslessly paged through private,
create-new SD files and removed on completion, cancellation or error. Paging
temporarily uses a 12 MHz SD clock and restores the previous clock afterward.
Keep the card inserted during decoding and poll cooperatively. USB debugging,
buttons, watchdog servicing and UART continue while JPEG scans run. Software
decoding has a 600-second deadline and is much slower than hardware decoding.
Large stb metadata and iterative GIF-prefix scratch use reserved extra RAM;
the decoder remains single-job and non-reentrant. The build checks compiler
stack frames, and native format tests check the separate 4 KiB stack guard.

Encoding accepts aligned NV21 planes and emits a complete baseline 4:2:0 JPEG,
including its JFIF header, quantization/Huffman tables and end marker. Source
pitch must be `round_up(source_width, 32)`; target width/height must be even
and at least the source dimensions. Encoder downscaling is currently rejected.
The initial quantization tables are quality 90, with stock selector 8; other
selectors are exposed for research and have no calibrated quality mapping.
Keep source/output buffers owned by the engine until `DONE` or `ERROR` and
call `apx_jpeg_poll` cooperatively. Decode input requires 32 zero guard bytes.
The engine uses a two-second deadline and stops DMA on completion, error or
cancellation. BootROM return, reboot and power-off also shut it down.

`appotech/jpeg_files.h` adds FatFs preview and create-new sample encoding.
It fits images to the screen with preserved aspect ratio and black borders.
The sample is a deterministic 220×176 pattern that the hardware scales to
220×176, 320×240, 640×480, 1280×720, 1280×960, 1600×1200, 1920×1080 or
2048×1536. Files are synchronized and closed before success is reported;
existing output files are refused. A failed write can leave its new partial
file for diagnosis.

```sh
make EXAMPLE=jpeg-demo
apxtool --device /dev/sg0 sdk launch build/jpeg-demo.manifest.json --reclaim-jpeg-ram
apxtool debug --port 5-1 jpeg info
apxtool debug --port 5-1 jpeg preview 0:/SDKJPEG/BASELINE/jpeg-test-220x176.jpg
apxtool debug --port 5-1 jpeg preview 0:/jpeg-test-220x176.jpg --allow-slow
apxtool debug --port 5-1 jpeg preview 0:/picture.png
apxtool debug --port 5-1 jpeg encode-sample 0:/SDKJPEG/ENCODER/new-3m.jpg 2048 1536
apxtool debug --port 5-1 sd get 0:/SDKJPEG/ENCODER/new-3m.jpg new-3m.jpg
apxtool debug --port 5-1 jpeg cancel
```

Use your camera's actual SCSI path and USB port. Launch from a fresh boot when
changing image layouts. `--reclaim-jpeg-ram` explicitly backs up occupied
vendor RAM in the fixed codec workspace beside the manifest, then clears and
verifies it; other reservations still require zero preflight. This build
reserves `0x022e0000..0x023fffff`: a 1 MiB stream and 128 KiB NV21 workspace.
JPEG code/tables share the existing `0x02220000..0x022dffff` extension with
FatFs Unicode tables. The 16 KiB heap and 4 KiB stack remain separate.

Software rendering (progressive JPEG, baseline 4:4:4, PNG, BMP, TGA, GIF and
PNM) is slower than the hardware baseline 4:2:0/4:2:2 path. By default it
accepts sources up to 220×176 pixels (either dimension at most 220), with
compressed files at most 128 KiB. High-precision JPEG quantization is also
gated: a table whose mean value is at most 8 is treated conservatively as
expensive; JPEG has no universal quality-percentage field. Larger or
higher-quality software images return `APX_JPEG_SLOW` before decoder allocation
or coefficient-file creation. Hardware-compatible JPEG retains all eight
supported sizes without this gate.

Opt in per image with `jpeg preview <path> --allow-slow`, or C
`apx_jpeg_preview_file_ex(path, APX_IMAGE_ALLOW_SLOW)` /
`apx_image_decode_start_ex(..., APX_IMAGE_ALLOW_SLOW)`. Opt-in retains the
2048×1536 and 1 MiB input hard limits. It can take minutes and can create
three temporary coefficient files on SD. On the tested Class 10 card,
progressive 640×480 took about 21 seconds, 1600×1200 about 85 seconds and
2048×1536 about 148 seconds. Smaller high-quality images can also be slow.
Use a high-quality Class 10 microSD, as recommended by the camera manual.
The unmarked 2 GB card stopped responding twice under 3 MP paging; this does
not yet distinguish card behavior from a controller compatibility issue.
Cancellation and recovery close/remove temporary files while the card responds.

The supplied card originals are progressive 4:4:4. They are preserved;
the software backend can display them directly. The tested hardware baseline
copies live under `SDKJPEG/BASELINE`. To prepare copies
from a different card, with Pillow installed, use a new host directory:

```sh
PYTHONPATH=../apxtool/src python3 tools/prepare_jpeg_baseline.py --port 5-1 --directory research/new-jpeg-inputs --upload
python3 tools/test_jpeg_model.py
python3 tools/test_jpeg_live.py --port 5-1 --output research/new-jpeg-live --seconds 60
```

The live test additionally needs numpy and Pillow, an active `jpeg-demo`, and
an existing `SDKJPEG/ENCODER` directory. It creates new output names containing
the build ID; supply a new `--tag` to repeat it without overwriting prior files.
`tools/test_jpeg_422_live.py` checks all eight 4:2:2 sizes, and
`tools/test_jpeg_recovery.py` checks same-image BootROM/relaunch cycles. See
[JPEG hardware evidence and limitations](research/jpeg.md) and
live results.
Software image and UART details are in [the progressive/UART report](research/progressive-uart.md).

## UART

`appotech/uart.h` provides explicit initialization, polling and bounded binary
TX/RX queues. The camera's exposed TX pad is **PA7, TX route 6**; its output
has been verified with a serial adapter at 9600, 115200 and 230400 baud, 8N1.
The exposed pad is TX-only. Receive handling is register-model tested;
physical RX routing remains unverified. Initialization is optional and does
not occur merely by linking the driver. Alternate mux selectors require
board-specific pin identification.

```sh
apxtool debug --port 7-1 uart configure --baud 115200 --tx-route 6
apxtool debug --port 7-1 uart write message.bin
apxtool debug --port 7-1 uart console on
apxtool debug --port 7-1 uart info
apxtool debug --port 7-1 uart console off
apxtool debug --port 7-1 uart stop
```

The optional console mirror sends stdout/stderr to UART without consuming the
USB diagnostic ring. It drops bytes if its bounded queue fills and reports
the count. Command responses stay on USB. Shutdown restores the saved GPIO
fields and control register; the write-only baud divider falls back to the
board's known stock 115200-baud setting. BootROM and power handoffs stop UART.

## Speaker audio

`appotech/audio.h` provides independent polling DMA playback of signed 16-bit
little-endian **mono PCM at 8, 16 or 32 kHz**. The source must be aligned DDR
and contain 32..65536 bytes in complete 32-byte cache lines. Keep it unchanged
until `DONE`/`ERROR`, or call stop before reusing it. The driver drains briefly
after DMA completion, then mutes the speaker; trailing silence and an envelope
help avoid abrupt transitions. Volume accepts 0..100%, and mute preserves the
chosen volume. These are finite-buffer jobs; continuous DMA queuing, stereo,
other sample rates and compressed output are not implemented. Microphone
recording uses the separate capture driver described below.

`appotech/audio_files.h` adds tones, a staged PCM upload service and read-only
WAV playback from SD. WAV input must be PCM16 mono at a supported rate, with
at most 65504 data bytes. RIFF/fmt/data boundaries and sizes are checked, the
file is closed before DMA starts, and a silent tail is appended. The 64 KiB
staging buffer shares the existing extra DDR reservation with FatFs tables and
JPEG code; it uses no heap allocation and is protected by normal APXD writes.

```sh
make EXAMPLE=audio-demo
apxtool --device /dev/sg0 sdk launch build/audio-demo.manifest.json --reclaim-jpeg-ram
apxtool debug --port 5-1 audio info
apxtool debug --port 5-1 audio volume 60
apxtool debug --port 5-1 audio tone --frequency 440 --milliseconds 1000 --rate 16000
apxtool debug --port 5-1 audio wav 0:/SDKAudio/example.wav
apxtool debug --port 5-1 audio pcm sample.s16le --rate 16000
apxtool debug --port 5-1 audio mute on
apxtool debug --port 5-1 audio mute off
apxtool debug --port 5-1 audio stop
```

Use a fresh boot when changing firmware layouts. The `audio-demo` retains the
native JPEG preview, USB debug, SD access, GPIO and Power-hold shutdown. It
starts silently; tone/WAV/PCM commands begin playback and wait for completion.
The optional audio poll hook runs from `apx_maintenance`, so SD, display and
JPEG waits continue to service playback without changing watchdog policy.
For a custom application, initialize once with `apx_audio_init`, use
`apx_audio_play`, and poll via maintenance or `apx_audio_poll`. BootROM return,
reboot and power-off stop DMA and mute/disable the DAC before teardown.

```sh
python3 tools/test_audio_model.py
python3 tools/test_audio_live.py --port 5-1 --elf build/audio-demo.elf --output research/new-audio-live.json --seconds 60
python3 tools/test_audio_recovery.py --port 5-1 --manifest build/audio-demo.manifest.json --output research/new-audio-recovery.json
```

The live test creates new files under `SDKAudio`, with build IDs in their names;
prior test files must be absent when repeating that test on the same build.
See [audio register evidence and limits](research/audio.md) and
acceptance.

## Microphone capture

`appotech/microphone.h` provides independent polling PCM16 little-endian mono
capture at **8, 16 or 32 kHz**, with start, completion, cancellation and
shutdown. Supply 32-byte aligned DDR in complete cache lines, 32..65536 bytes;
keep it untouched while capture is active. Completed data is invalidated for
CPU access. Speaker output can run concurrently in a disjoint buffer, and both
drivers reject overlapping active DMA ranges.

Initialize `apx_microphone_files_init` to link the optional guarded 64 KiB
capture workspace and host/file service into a custom application. It supports
host PCM retrieval, new SD WAV files and playback of the completed recording.
The buffer and its 64-byte guards use the existing extra DDR reservation.
Captures are finite: up to 4.096 s at 8 kHz, 2.048 s at 16 kHz or 1.024 s at
32 kHz. The current gain/analog configuration is the traced stock preset;
initial samples can contain a startup transient, and nearby loud claps can
clip. Continuous queuing, stereo, gain/AGC tuning and compressed recording
remain deferred.

The existing SD WAV playback service reserves a silent tail and accepts at
most 65504 data bytes. Choose capture sizes up to 65504 for later SD WAV
playback; live microphone replay supports the full 65536-byte capture.

```sh
make EXAMPLE=microphone-demo
apxtool --device /dev/sg0 sdk launch build/microphone-demo.manifest.json --reclaim-jpeg-ram
apxtool debug --port 5-1 microphone info
apxtool debug --port 5-1 microphone capture --bytes 64000 --rate 16000 --output recording.wav
apxtool debug --port 5-1 microphone save 0:/SDKAudio/recording.wav
apxtool debug --port 5-1 microphone get another-host-copy.wav
apxtool debug --port 5-1 microphone play
apxtool debug --port 5-1 microphone stop
```

Host and card WAV saves refuse existing files. Card saves sync and close the
file; a storage error can leave a partial new file and is reported as failure.
The demo keeps JPEG/display, SD, USB debugging, buttons and Power-hold shutdown
working. It starts without an active capture. Use a fresh physical boot when
switching firmware layouts, as for the speaker demo.

```sh
python3 tools/test_audio_model.py
python3 tools/test_microphone_live.py --port 5-1 --manifest build/microphone-demo.manifest.json --output research/new-microphone-live.json
python3 tools/test_microphone_recovery.py --port 5-1 --manifest build/microphone-demo.manifest.json --output research/new-microphone-recovery.json
```

The live test creates a dedicated `SDKAudio/microphone-<build-id>.wav` file;
use a card where that file is absent when repeating it. See
[microphone register evidence](research/microphone.md) and
acceptance.

## Camera capture and live preview

`make -C appotech-sdk EXAMPLE=camera-demo` builds independent sensor I²C/reset/clock and DVP
receive DMA, hardware-scaled 220×176 LCD preview, native 640×480 NV21 retrieval, hardware
JPEG photos and create-new SD saving. USB debugging, SD, buttons, watchdog,
speaker and microphone remain available in this example.

```sh
apxtool --device /dev/sg0 sdk launch appotech-sdk/build/camera-demo.manifest.json --reclaim-jpeg-ram
apxtool debug --port 5-1 camera capture --output photo.jpg --save 0:/photo.jpg
apxtool debug --port 5-1 camera preview on
```

The sensor's verified native resolution is VGA; larger photos use the existing
JPEG upscaler. Small 220×176/320×240 photos are reduced in software.
Preview uses two guarded buffers with camera scaler DMA and compositor/LCD DMA,
leaving the sensor running between frames. At the default 144 MHz profile it
sustained 12.39 frames/s with audio/microphone and SD active, versus 8.27 frames/s
with DMA at 96 MHz and 2.77 frames/s with the original software resizing path.
See [clock validation](research/clock.md). Sensor exposure/scene timing can affect
frame rate.
Hardware encoder downscaling remains deferred. Progressive JPEG is available
through the software image backend described above. The
camera and JPEG file-demo services use separate examples because their static
buffers share the codec RAM reservation; the reusable JPEG decode/encode core
remains available to applications. See [camera bring-up and evidence](research/camera.md),
[public capture API](include/appotech/camera.h) and
camera DMA acceptance.

## Battery indicator and SPI NOR

The camera demo shows a five-segment voltage indicator at the top right of
live preview. `+` means external power present. Battery samples share the ADC
with the buttons; the SDK uses the stock calibration, voltage thresholds and
30mV hysteresis. The API reports valid millivolts and coarse levels, not a
state-of-charge percentage. Charger settings are left to the inherited board
configuration.

```sh
apxtool debug --port 5-1 battery
apxtool debug --port 5-1 flash info
apxtool debug --port 5-1 flash read spi-backup.bin
apxtool debug --port 5-1 flash read header.bin --address 0 --length 512
```

`appotech/battery.h` provides init/poll/status/shutdown; `appotech/adc.h` arbitrates
conversions between battery and buttons. `appotech/spi.h` provides the independent
controller driver, JEDEC/status reads, arbitrary byte-range reads with guarded
DMA, and explicit C page-program/sector-erase calls. APXD also exposes page
programming and sector erase with an `APXF` marker on every destructive frame.
The host installer creates a full verified backup, checks the expected current
SHA256, verifies each sector and then the entire chip, and commits boot sector
zero last. Read/download commands refuse existing output files. See SPI boot
below before replacing the vendor application.

The battery overlay is implemented by the camera demo's optional
`apx_camera_preview_overlay` hook on completed frames before LCD DMA; captured
photos are unaffected. See [register evidence and API details](research/battery-spi.md),
acceptance results and
[driver headers](include/appotech/spi.h).


## SPI boot: camera diagnostic preview

Build the diagnostic preview and package it using the **original board backup**:

```sh
make EXAMPLE=camera-demo BUILD=build/spi-boot-camera
python3 tools/spi_image.py --original <your-stock-chip-backup>.bin \
  --manifest build/spi-boot-camera/camera-demo.manifest.json \
  --output build/spi-boot-camera/new-camera.spi.bin
```

The image is exactly 2 MiB and is specific to the verified JT5295 board and
bootstrap hash. The packer refuses unknown bootstraps, invalid segment hashes,
out-of-range layouts and existing output paths. It preserves the vendor's
8 KiB SRAM bootstrap for board-specific RAM/PLL calibration. It changes the
BLDR DDR destination/count and the bootstrap's final call target, replacing
the vendor application with a contiguous SDK image. SDK USB and peripheral
drivers remain independent of vendor application routines.

At cold entry the SDK restores the bootstrap's saved ROM stack page, clears
NOLOAD DMA/work buffers, normalizes the known incoming PLL/timer profile and
starts the usual 144 MHz runtime. USB initialization now enables its PHY/core
clocks explicitly. INFO reports `boot_source: "spi"`. The default application
shows live DMA preview with the battery indicator and supports USB diagnostics,
SD access, capture, audio, buttons, watchdog and graceful power-off.

Launch the same build through USB before installing it, then replace SPI:

```sh
apxtool --device /dev/sgX sdk launch build/spi-boot-camera/camera-demo.manifest.json --reclaim-jpeg-ram
apxtool debug --port 7-1 flash install build/spi-boot-camera/new-camera.spi.bin \
  --backup research/new-pre-install.bin \
  --expect-current-sha256 <full-current-SPI-SHA256>
```

`--backup` must name a new file; its adjacent `.json` journal records progress,
verified sectors and the full readback hash. Installing a full 2 MiB original
backup through the same command restores the vendor firmware; supply the
current SDK image's hash and a new backup filename. All firmware is loaded into
RAM before use, so the independent SDK SPI driver can rewrite the boot image.
`apxtool flash` and `apxtool rom-rescue` do not depend on the installed firmware either:
they upload `boot/flasher.c` (`make -C boot`) and drive the SPI controller themselves, so they also
work after the vendor application has been replaced. Use `apxtool debug ... flash` while an
SDK is running.

For normal recovery, `apxtool debug --port 7-1 bootrom` acknowledges first and
returns to the ROM SCSI loader. A valid SPI SDK image also checks SHUTTER during
the first 250 ms of SDK startup: hold it while powering on to enter BootROM
before display, sensor, SD or audio initialization. From the ROM loader use
`sdk launch` with the RAM SDK manifest to regain the independent flash driver.
A physical power cycle is still required when switching RAM code layouts.

Keep the original backup outside the camera. Flash replacement is not atomic;
power loss, a damaged bootstrap or failure before the shutter check can require
an external SPI programmer. The shutter path depends on a working SDK startup.
See SPI boot evidence, hashes and validation.

## Cold-boot diagnostics and failure handling

A cold power-up has no USB, display or surviving RAM, so a silent failure is
indistinguishable from a dead board. Early SPI-boot failures (invalid saved ROM
stack, rejected incoming clock profile, clock init error) now fall into the
BootROM loader instead of resetting. Build with `APX_SPI_TRACE=1` for a flight
recorder in the last SPI sector and on the UART TX pad; see
[`include/appotech/trace.h`](include/appotech/trace.h) and
[recovery notes](research/spi-boot-recovery.md). Decode with `apxtool trace`.

## SPI flash map

How the 2 MiB chip is laid out, how much each example leaves for application data, how to use it from the SDK
and how to get more: [`research/spi-flash-map.md`](research/spi-flash-map.md). `python3 tools/flash_map.py IMAGE.bin` prints the
map of any packed image or chip dump.
