# JT5295 evidence ledger

The independent ILI9225G display milestone is documented in
[`display.md`](display.md), with validation in
`display-acceptance.json`. Its optically verified
board orientation is 220×176 landscape over native 176×220 GRAM.

The independent SD/FatFs milestone is documented in [`sd.md`](sd.md), with
validation in `sd-acceptance.json`.

## Images and mappings

The live pre-test 2 MiB SPI capture is `spi-before.bin`, SHA-256
`063e10e7df19095f5ee8217cb031e03297941194f460959a47056c12a38dab66`.
BootROM is [`jt5295-rom-00100000-64k.bin`](https://github.com/matu6968/apxtool/blob/main/research/jt5295-rom-00100000-64k.bin) in the apxtool repository, SHA-256
`f5b68ff45bff5f4f21c49008ed0a4bcbab01f2a2cb5aeff8f832ee41be460e39`.
BootROM maps at `0x00100000`; main SPI offset `0x2200` maps to RAM
`0x02000000`. The verified RAM window is `0x02000000..0x023fffff`.
SDK ownership is limited to its two explicit reservations, not the whole window.

## CPU/ABI: hardware verified

`abi-assembly-v2.json` records little-endian byte/halfword/word accesses,
skipped taken branch slots and a call at `0x02180060` linking to
`0x02180064`. `c-abi-O0.json`, `c-abi-O2.json` and `c-abi-Os-v2.json` record
zero failure bits with restoration verified. They cover globals, struct layout,
function pointers, seven arguments including stack arguments, software multiply,
signed/unsigned division, 64-bit division and software double arithmetic,
integer formatting, allocation, exhaustion and nonlocal returns.

The earlier `c-abi-Os.json` failed double arithmetic (bit 0x40), with RAM
restored. The cause was libgcc's OpenRISC soft-float configuration hardcoding
big-endian words. It now uses the compiler's byte-order macro. Preserve this
failed record as evidence for the correction.

`c-abi-O0-v2.json`, `c-abi-O2-final.json` and `c-abi-Os-v3.json` pass the
expanded checks. The additional native probes cover explicit 64-bit returns, nested struct
arguments/returns, signed `char`, unaligned libc copy/move and 64-bit formatting.
An expanded formatter test initially failed (bit 0x2000) because the integer
formatter default disables `long long`. Enabling Meson `io-long-long=true`
resolved it without enabling floating-point formatting.

Toolchain smoke tests compare LE/BE instruction/data encoding and HI16, LO16,
REL26 and ABS32 relocations (`toolchain-check.json`). These are focused tests,
not the full upstream GCC/binutils test suites.

## USB: raw ROM evidence and hardware validation

`rom-usb-full.disasm` and `rom-usb-transfer.disasm` are raw instruction
listings. `USBTrace.java` and `ghidra-usb-trace.json` capture the MCP script
inspection. The older Ghidra language modeled delay slots incorrectly; raw
instructions and hardware probes take precedence over those decompilations.

| Evidence | Interpretation | Confidence |
| --- | --- | --- |
| `0x00105d20..0x00105f70` | PHY/clock and controller initialization | ROM verified; electrical meanings incomplete |
| `0x00106794` register pointer table | USB byte register offset mapped to SPR `0xa000 + 4*offset` | ROM and live driver verified |
| EP0 setup copying from address `0x8000` | Eight-byte setup buffer in mapped FIFO memory | ROM and enumeration verified |
| EP0 SPR `0xa080`, EP1 SPR `0xa090` | Byte FIFO accesses | ROM and bulk traffic verified |
| SPR `0x984c/0x9850/0x9854` | FIFO mapping values `0x8000/0x8040/0x8240` | Sequence verified; capacity mapping not fully resolved |
| SPR `0xd840/0xd844/0xd848/0xd84c/0xd850` | Vendor transfer/DMA engine | ROM sequence verified; SDK uses PIO |
| SPR `0x4800` bit 15, status SPR `0x18` bit 1 | Vendor external USB interrupt enable path | ROM verified; disabled during takeover |
| SPR `0x880c = 0x5001d` | Repeated maintenance write | Watchdog reload/enable, selector 5; [hardware timing and power evidence](watchdog-power.md) |
| POWER bit 4, Linux sysfs speed `480` | High-speed negotiation | Hardware verified |

The driver inherits the initialized PHY from the loader, disconnects, stops
the transfer engine, disables external interrupts, resets the controller and
sets up its own endpoints. This is RAM takeover, not a cold-boot PHY driver.
Its USB operation contains no BootROM or vendor calls. Recovery is separately
allowed to jump to loader entry `0x0010385c` using the saved ROM stack.

The first recovery re-enumerated BootROM but lost its final host ACK.
Adding a 100 ms grace period before disconnect resolved this. BootROM can retain
the SDK's reservations, so the host now releases those regions only after the
ACK, physical-port match, loader INQUIRY and successful RAM read. Ten complete
cycles then passed (`launch-cycles-v2.json`). The occupied-RAM rejection in
`launch-cycles.json` is retained as evidence that preflight worked.

`abi-O2-accepted.json` records a timed-out returning callback and unverified
restoration during back-to-back layout changes after the O0 probe. The owner
physically reset the camera. The identical ELF/manifest then passed with zero
failures and verified restoration (`abi-O2-cold.json`). This points to a warm
loader/cache/context dependency; it does not isolate its cause. Layout-changing
ABI probes now require fresh stock firmware entry. Changing builds in a warm
loader is not an accepted capability. Same-image SDK cycles passed.

## Ghidra

`OpenRISC:LE:32:JT5295` is a separate language: LE code/data, no delay slots,
PC+4 call link and little-endian 64-bit return register join. Generic variants
remain unchanged. Headless ELF import plus `VerifyJT5295.java` checked 69 call
links and 477 branch/call instructions in the final image with no delay slots. Installed language
definitions become available in the running GUI after restarting Ghidra.
The existing vendor programs have not had their language or annotations changed.
Unverified SPR and custom-opcode semantics remain inherited/unknown.

## Acceptance records

Live records document descriptor/halt/protection/boundary checks, negotiated
speed, transfer duration and byte counts. BootROM recovery, repeated cycles,
full-speed fallback, suspend/resume, reset and SPI comparison each have their
own records. Read their status fields before treating a check as passed.

The final aggregate is `acceptance.json`:

| Check | Result |
| --- | --- |
| Native ABI/libc at O0/O2/Os | Zero failures, verified restoration; O2 uses the owner-confirmed cold-reset run |
| HS sustained transfers | 600.003 seconds, 146,188 cycles, 640,961,286 verified bytes |
| FS fallback | Linux reports 12 Mb/s; descriptors, boundaries and halt recovery pass |
| Reset/reconnect | HS and FS pass |
| Suspend/resume | Linux policy test passes; firmware suspend/resume counters increase |
| Launch/debug/BootROM | Ten acknowledged cycles, loader INQUIRY/RAM read and reservation release each time |
| SPI preservation | Full 2 MiB comparison passes, SHA-256 unchanged |
| Host tests | 113 tests and 6 subtests pass |
| Direct calls/inert slots | 55 direct calls confined to SDK code; 511 inert slots |
| Runtime footprint | 42,040 bytes plus separate 4,096-byte stack; heap 16,384 bytes |

The SDK is left running at physical port `7-1` with build ID
`dc20c05ee1965e6e`. `runtime-clocks.json` captures PLL1 control `0x1623`,
coefficient `0x0b71b000`, counter reload `0x3fff`, and an observed counter rate
near 32.75 kHz, consistent with the inferred 32.768 kHz reference.
The root-hub suspend method was rejected by this host (`usb-fs-suspend.json`);
the kernel-managed camera-only policy test succeeded and restored `on`/2000 ms.

## Hardware JPEG milestone (2026-10-14)

The current accepted `jpeg-demo` is build `7b801bc3d5be10f8`, ELF SHA-256
`588ca05a3d27882e02efdaacbaebd47f7bf2eefff0fb8285e1e85c268fd4b775`.
It is left running at physical port `5-1`, independently decoding the native
220×176 SD test image to the LCD; the user confirmed correct colors and no
scrambling. USB remains high speed, automatic watchdog selector 5 is active,
and the final display/SD/ADC timeout/error counters are zero.

The shared `c000` JPEG core independently encodes and decodes; no vendor
codec routine is called. Baseline 420 and 422 decode pass at all eight source
resolutions through 2048×1536, with display-sized NV21 output. Baseline 420
encode and decode/display round trips pass at all eight target resolutions.
Progressive, 444 and other unsupported formats are rejected before hardware
start. A progressive hardware path is not verified. The original card files
are progressive 444 and remain hash-identical; separate baseline copies are
used. Encoder downscaling remains rejected; the sample starts at 220×176 and
uses hardware scaling to larger targets.

Live acceptance includes 52 decode/display jobs in 61.48 seconds, malformed
and unsupported input rejection, create-new output protection, cancellation,
zero stack-bottom guard, and three final same-image BootROM/relaunch cycles.
The full 2 MiB SPI remains unchanged, SHA-256
`063e10e7df19095f5ee8217cb031e03297941194f460959a47056c12a38dab66`.
The SPI readback callback was run separately from a fresh stock entry because
switching callback layouts in a warm loader remains unsafe/unverified.
Intermediate failures and required physical resets are retained in the JPEG
research record. See [JPEG evidence](jpeg.md),
combined acceptance,
live image tests,
422 matrix, recovery and
final health.

## Speaker audio milestone (2026-10-14 to 2026-10-15)

The accepted `audio-demo` build is `37f27e3d3ef105c2`, ELF SHA-256
`fa0154b439864aad92c1de928862084c55404d6fb5e8e17917f11676eb69ff23`.
It owns the speaker DAC/DMA at `9140..9160` and its analog controls directly;
no vendor audio routines run. Signed 16-bit little-endian mono PCM is verified
at 8/16/32 kHz. Tones, gain/mute, finite-buffer playback, staged host PCM and
read-only SD WAV playback are available. Capture, stereo, compressed formats,
other rates and continuous DMA queuing remain deferred.

The user confirmed alternating 440/660 Hz tones at 60% were audible and clean.
Acceptance covers PCM boundaries 2..65504 bytes, source/padding readback,
WAV parsing/negative cases, cancellation, volume and active mute/unmute.
During 60.64 seconds, 43 audio jobs ran alongside 43 large JPEG previews;
USB, SD, buttons/ADC and automatic watchdog remained responsive with zero
audio/display/SD/ADC timeout/error counters. Three BootROM/relaunch cycles
passed while speaker DMA was active. Active-audio power-off acknowledged and
disconnected USB after 102.6 ms; the user confirmed normal off/on behavior.

All 145 host tests and six subtests pass, as do audio/system/JPEG/display/
SD/FatFs/GPIO models with sanitizers. ELF audit passes 1045 direct calls
confined to SDK executable sections and 4395 inert branch slots. Runtime uses
121616 bytes including the 16 KiB heap; stack remains 4 KiB. The additional
64 KiB PCM workspace and audio code fit the already reserved extra DDR bank.
Total RAM reservations are unchanged from `jpeg-demo`.

The final restored build also passed largest-image JPEG encoding, SD save,
full host decoding and hardware playback alongside audio. The host restores
its normal five-second request deadline after SDK readiness probing; the
regression covers the original one-second timeout and successful retry with
the corrected returned connection, without a camera reset.

The full 2 MiB SPI readback remains unchanged at SHA-256
`063e10e7df19095f5ee8217cb031e03297941194f460959a47056c12a38dab66`;
SPI callbacks were run separately from a fresh stock entry, followed by a
physical reset before restoring the final audio SDK. See
[audio evidence](audio.md), combined acceptance,
live tests, recovery,
power-off, SPI readback
and final health.

## Microphone milestone (2026-10-15)

The accepted `microphone-demo` is `7e2e6a4d3a040a36`, ELF SHA-256
`0f6bda4e3ef4e9e6c098deaf58d70c2f08cda4b1eed29acb88044e897a0c67dd`.
It owns recording SPRs `9100..9120` and polls full-buffer completion while
keeping IRQ 18 masked. Verified PCM16 mono rates are 8/16/32 kHz with the
speaker clock unchanged. Independent speaker and recording DMA operate in
disjoint DDR buffers; overlapping ownership is rejected. Capture/host WAV
retrieval, guarded finite buffers, new SD WAV saving and recording playback
are available. The user spoke/clapped and confirmed replay was clear.

Native tests cover exact rate timing, 32..65536-byte transfer boundaries,
half/full DMA, guards and data readback, cancellation/malformed input, WAV
creation/readback and existing-file protection. Captured speaker tones peak
at the expected 440/660 Hz. In 60.69 seconds, 37 capture/speaker jobs and
37 large JPEG previews completed with 151552 verified SD bytes and no error
counters. Three BootROM/relaunch cycles passed with both DMA engines active.
Manual watchdog feeding and largest-image JPEG encoding also passed during
recording; the stack-bottom guard remains clear.

The actual-core/file-service sanitizer model and system model pass, along
with 147 host tests and six subtests. ELF audit passes 1116 direct SDK calls
and 4601 inert branch slots. Runtime is 121800 bytes including the 16 KiB heap;
the added 64 KiB plus guards fit existing extra DDR, with no new reservations.
No SPI program/erase operation was performed; the preceding speaker full-SPI
readback remains the last flash-preservation measurement. Continuous capture,
gain/AGC tuning, stereo and compressed recording remain deferred. See
[microphone evidence](microphone.md), acceptance,
live tests, voice clips,
recovery and final health.
