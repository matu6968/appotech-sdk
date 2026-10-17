# JT5295 microphone capture

The SDK owns the recording controller at SPR `9100..9120` directly. The
public `appotech/microphone.h` API captures finite PCM16 little-endian mono
buffers at 8/16/32 kHz. `appotech/microphone_files.h` adds a guarded 64 KiB
workspace, host retrieval, new SD WAV files and speaker replay. Speaker and
microphone DMA can run simultaneously in disjoint buffers. USB/debug, JPEG,
display, SD, GPIO and automatic watchdog service continue while recording.
The original timing evidence below used 96 MHz. Current SDK builds default to
144 MHz with separately verified rate controls, including 8 kHz's extra divider;
see [SDK clock profiles](clock.md).

## Firmware and hardware evidence

`microphone-image-evidence.json` pins `flash.bin.golden`, its file-to-RAM bias
`01ffde00`, and the traced regions. The recording instructions are in
`audio-vendor-raw.disasm`; `microphone-vendor-high.disasm` extracts the stock
buffered recording service. Ghidra script execution matched raw bytes and
`AnnotateMicrophone.java` labels the relevant sequences. Generic OpenRISC
delay-slot analysis did not recover their direct callers; signed native
PC-relative instruction decoding found them instead. Generic function bodies
were not changed. The supplied rear PCB photograph shows the microphone
beside the LCD connector; exact SoC pad/bias wiring remains undocumented.

| Address | Traced behavior |
| --- | --- |
| `02027c24` | IRQ 18: bit 1 half-buffer callback, bit 0 completed-buffer callback |
| `02027cf4` | Recording enable/stop and FIFO reset |
| `02027dec` | Analog/filter/AGC configuration, `9118/911c/9120` |
| `02027e7c` | Four stock gain/control parameters, all supplied as 5 |
| `02027ef8` | Destination `9108`, word count `910c`, destination cache invalidate |
| `02027f58` | Shared clock gates/routing and analog enable |
| `02027ffc` | Stock sample-rate setup |
| `02034fc0` | Buffered recording start; `020351f0` stops it |

The microphone initialization uses the speaker core's existing shared clock
ownership. It masks PIC IRQ 18, saves passive recording/analog fields, and
enables `886c` bits `3c1f`. It does not touch the button SAR ADC at `9170`.
Recording uses the stock analog/gain preset; arbitrary gain and AGC tuning
are not exposed because exact field meanings have not been verified.

| SPR | SDK use |
| --- | --- |
| `9100` | Rate control plus low enable/analog/FIFO/DMA bits |
| `9104` | Stock `007c0002` configuration |
| `9108` | Byte-addressed DDR destination; readback verified |
| `910c` | Words minus one; readback is a programmed length, not remaining samples |
| `9110` | Stock `780a0005` |
| `9114` | Reset strobe bit 31, reset-ready bit 2; half bit 1, complete bit 0 |
| `9118` | Stock `5553330f`; low bits 0/3 cleared on stop |
| `911c` | Stock `40003000` |
| `9120` | Stock `01007800` |

DMA setup cleans/invalidates the destination, writes address/count, sets
control bits 2/3, resets the FIFO and waits at most 10 ms for ready. It clears
the ready event, enables control bit 0 and writes `9114=30000`; PIC 18 remains
masked. Polling waits for the **full** buffer flag, quiesces the controller,
then invalidates the complete destination before reporting `DONE`. Normal
stop clears DMA address/count and analog enables. Timeout is expected buffer
duration plus 500 ms. Shutdown restores passive owned fields after stopping;
speaker clock shutdown also shuts down recording first.

## Verified format and timing

The first stock-32-kHz probe reset successfully but never wrote a buffer.
The stock-16-kHz setup captured at roughly 10.667 kHz in the inherited clock
profile. An early probe also mistook the half event for completion and left
the second half unchanged. Both errors are retained in the probe records.
On-chip scratch mapping was not verified and was abandoned; production uses
DDR exclusively. Failed probes restored the reviewed scratch regions and
passive registers; no vendor recording routines were executed.

Changing shared clock divider `b` to `7` showed proportional timing, but would
also affect speaker rates. Final controls **001f4000 / 000f4000 / 00074000**
produce **8 / 16 / 32 kHz** with the shared divider **b unchanged**. The
prescaler interpretation follows measured timing; the complete audio clock
tree is still undocumented. Changing `9104`'s low divisor field did not change
the observed sample rate, and no meaning is assigned to it beyond the stock
sequence. Final native timing uses the SDK's inferred 32.768 kHz reference,
not an external acoustic/oscilloscope clock measurement.

The destination must be 32-byte aligned DDR, 32..65536 bytes, in complete
cache lines, and remain untouched while `CAPTURING`. Speaker/microphone APIs
reject overlapping active DMA buffers in both directions. The optional
service checks 32-byte guards before and after its workspace, with cache
invalidation before checking them. Incomplete/cancelled/failed recordings
cannot be downloaded, played or saved as valid clips.

There is an analog/filter startup transient in initial samples. Speech
measurements/clip selection skip the first 200 ms; saved data remains the
unaltered hardware PCM. Loud nearby claps can saturate PCM16 with the stock
gain preset. There is no host normalization or software resampling.

## APXD and file integration

APXD v1 retains its 24-byte header and 4096-byte payload limit. Operation
**10 (MICROPHONE)** provides:

| Command | Request |
| --- | --- |
| 0 | Empty: JSON state, rate, bytes, events/ticks/errors, buffer and guard status |
| 1 | `u32 bytes, rate`: start guarded capture |
| 2 | Empty: stop and discard incomplete recording |
| 3 | `u32 offset, length`: completed PCM, even alignment, at most 4096 bytes |
| 4 | NUL-terminated UTF-8 path: create new PCM WAV on SD |
| 5 | Empty: play completed recording through the speaker |

Recording errors use `400+mic_error`; storage errors use `100+FResult`.
WAV output has the captured sample rate and exact complete sample count.
SD saves use `FA_CREATE_NEW`, bounded writes, sync and close; existing files
are protected. A card error can leave a partial newly created file, which is
not reported as successful. Host WAV output also refuses existing files.
Capture is refused while the speaker owns the recording buffer for replay;
playback from other buffers can run concurrently.
The existing SD WAV reader accepts up to 65504 data bytes to reserve its
silent tail; a full 65536-byte recording can be replayed directly, but should
be captured at 65504 bytes or less for later playback through that reader.

The 65536-byte capture buffer plus 64-byte guards share the existing extra
DDR bank with the speaker buffer, FatFs tables and codec code. No new RAM
reservation or heap allocation is needed. The demo retains native SD JPEG
preview and Power-hold shutdown and starts with no active recording.
Initialize the optional host/file service with `apx_microphone_files_init`;
this also ensures the strong module is linked into a static SDK application.

## Validation records

`microphone-first-native.json` records the integrated one-second capture at
16 kHz: exactly 32768 reference ticks, both guards intact and USB/JPEG still
working. The user spoke and clapped during repeated two-second recordings,
then confirmed the speaker replay contained a clear, understandable voice.
`microphone-voice/capture.json` records those clips and the replay.

The sanitizer model uses the actual audio/microphone cores and file services;
it checks half/full events, cache invalidation, disjoint/overlapping DMA,
unsupported rates, alignment/overflow, cancellation, FIFO/transfer deadlines,
shutdown, WAV creation/close, short writes/sync failures and guard damage.
The host suite passes 147 tests and six subtests. See the model/host logs,
`microphone-live.json`, `microphone-recovery.json` and combined
`microphone-acceptance.json` for final hardware/build-bound results.

The final native record includes three timing trials per rate, complete-tail
checks at 32/64/96/512/4096/8192/65536 bytes, captured 440/660 Hz tones during
simultaneous playback, byte-exact WAV saving/readback and existing-file refusal.
Over 60.69 seconds, 37 capture/speaker jobs ran with 37 large JPEG previews and
151552 verified SD bytes. Three BootROM/relaunch cycles passed with both DMA
engines active. A four-second recording survived manual watchdog feeding;
the largest JPEG encoded/saved/round-tripped while capture ran. Error counters
and the stack-bottom guard remained clear. Final code: `7e2e6a4d3a040a36`, ELF
SHA-256 `0f6bda4e3ef4e9e6c098deaf58d70c2f08cda4b1eed29acb88044e897a0c67dd`.
The runtime uses 121800/131072 bytes including the unchanged 16 KiB heap;
total reservations remain 2166784 bytes. No SPI program/erase operations were
performed in this microphone milestone; the preceding speaker milestone's
full SPI preservation readback is retained separately.

Continuous capture, ping-pong/ring queuing, stereo, other rates, USB Audio
Class, gain/AGC tuning and compressed recording remain deferred. Finite clips
are at most 4.096 s at 8 kHz, 2.048 s at 16 kHz or 1.024 s at 32 kHz.
