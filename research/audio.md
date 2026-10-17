# JT5295 speaker output

The SDK owns the speaker DMA/DAC path directly. `appotech/audio.h` provides
finite PCM playback, volume, mute, polling, cancellation and shutdown;
`appotech/audio_files.h` adds tones, staged host PCM and read-only SD WAV
playback. USB/debug, display/JPEG, SD, GPIO and watchdog service continue while
audio runs. The later [microphone driver](microphone.md) adds recording;
continuous DMA queuing remains deferred. This file records the speaker
milestone and its accepted build separately. Its rate measurements below used
96 MHz. Current SDK builds default to 144 MHz and use clock-dependent selectors;
see [SDK clock profiles and rate validation](clock.md).

## Raw firmware evidence

The main SPI mapping remains file offset `2200` → RAM `02000000`. Native
little-endian/no-delay disassembly is authoritative. Relevant stock sequences
are extracted in `audio-vendor-raw.disasm`; Ghidra labels/comments are applied
by `AnnotateAudio.java` and recorded in `audio-ghidra-annotations.txt`. Function
boundaries from generic OpenRISC delay-slot analysis remain unverified, so
the annotation script only creates labels. The unrelated ARM game-console
audio driver elsewhere in this workspace was not used for JT5295 registers.

| Stock address | Behavior | Confidence |
| --- | --- | --- |
| `020274a4` | DAC indexed writes at `915c`, busy bit 31; read result `9160` | Raw sequence and native playback |
| `020275b8/02027628` | Rate selection / DAC register 0 enable | Raw, codec readback and timed DMA |
| `020276ac` | Gain `32767*percent/100`, register 3; amplifier low bits at `8874` | Raw and audible volume/mute tests |
| `02027724/02027744` | PCM source/count and next-buffer setup | Raw; finite-buffer DMA verified |
| `02027788/02027804` | DAC start/stop, events `9150`, IRQ 6 | Raw and completion/cancel/recovery |
| `020278e8/02027958` | Filter/analog coefficient setup | Raw table replay in independent driver |
| `020279c8` | Clock routing and analog power sequence | Raw and live GPIO/USB/display coexistence |
| `0202e8ec/0202e944/0202e9f0` | Playback initialize/start/stop | Raw plus independent SDK equivalents |
| `0205d224` | 27 big-endian stored filter coefficients | Raw bytes and native tone/WAV playback |
| `0205e1c8` | Stock rate selector lookup | Raw; SDK timing differs as described below |
| `02027c24..020280dc` | Separate `9100` recording path, IRQ 18 | Raw only; not initialized by this driver |

No vendor or BootROM audio routines are called after takeover. The speaker
pads appear to route into the SoC on the supplied PCB photos, consistent with
the integrated analog controls; exact pad/rail naming is not established.

## Controller and analog sequence

The playback block is `9140..9160`, distinct from the recording block
`9100..9120` and the button ADC at `9170`. The DAC uses:

| SPR | Observed use |
| --- | --- |
| `9140` | Base control `10020` during setup; `202c` for playback |
| `9144` | Transfer length in words: `bytes/4-1` |
| `9148` | PCM source address; reads back zero on this board |
| `914c` | Event clear/stop `7`, start strobe `8`; readback is not a run flag |
| `9150` | Event flags; bit 2 signals DMA completion |
| `9158` | Available/remaining halfwords; starts at `bytes/2`, ends at 7 |
| `915c` | Indexed control: `(index<<24) | 40000000 | value` for writes |
| `9160` | Indexed read result; select with `915c=index<<24`, without bit 30 |

Reading `9160` without selecting a read transaction returns stale data. The
initial probe accidentally replaced the rate/enable register with a rate
alone and was silent even though DMA advanced. Explicit read/modify/write
preserves register 0 bit 0; the next probe was audible. The final SDK driver
and model test enforce this sequence. Gain changes bracket register 3 with
register 2 writes and program register 4, matching stock.

The driver masks vendor IRQ 6 and uses cooperative polling. It opens active-low
gates 24/30, clears clock-select bits 8/9 in `8848`, and sets `884c` bits 24..27
to `b`. Analog startup follows `886c`, `8870` and `8874` writes, then loads 27
filter coefficients and indexed registers 10..14. The low amplifier bits
`8874=..bb` enable audible output; zero mutes them. Unrelated register fields
are preserved where the traced operations use masks. Indexed busy waits have
a 10 ms deadline and continue automatic watchdog/maintenance service.

The recording DMA/control registers are not touched. Analog resource sharing
beyond the tested buttons/ADC path remains undocumented. Shared clock/analog
state is restored on final shutdown; normal stop mutes/stops DMA while keeping
the initialized clock path available for the next playback. A prototype USB
handle was lost during an intermediate register-level probe/cleanup; the same
SDK re-enumerated and was reopened/muted. Its exact cause is unverified.
The integrated run completed without that transport failure.

## Sample rates and format

The stock selector table is
`48000,44100,32000,0,24000,22050,16000,0,12000,11025,8000`.
Under the inherited SDK clock configuration, the counter consumes PCM at
approximately two thirds of those labels. The initial selector-6 probe ran
near 10667 samples/s rather than 16000. The cause is inferred clock routing/
divider dependence; a complete audio clock-tree description is not claimed.

Board selectors **8/4/0** are therefore used for **8/16/32 kHz** in this
configuration. Native timing is recorded in `audio-live.json`:

| Requested rate | Measured counter rate, three trials |
| --- | --- |
| 8000 | 8002.41–8003.14 samples/s |
| 16000 | 16006.56 samples/s |
| 32000 | 32011.93–32013.88 samples/s |

Measurements use the DAC count and the inferred 32.768 kHz reference ticks;
they are not oscilloscope/acoustic sample-clock measurements. Polling and FIFO
completion account for a small timing uncertainty. Selectors/rates 44.1/48 kHz
and arbitrary changed CPU/PLL configurations are not supported by this profile.

Input is signed 16-bit little-endian mono. The buffer must be 32-byte aligned
DDR and hold 32..65536 bytes in full cache lines; it is cleaned before DMA and
owned until completion/error/stop. Unsupported rates, alignment, overflow and
out-of-RAM ranges are rejected before hardware start. The driver adds a brief
16-sample drain interval on completion, then clears DMA pointers and mutes the
speaker. Deadline is expected duration plus 500 ms. Undocumented hardware
error bits are not invented; lack of completion becomes a bounded timeout.
Continuous refill/ping-pong behavior is not yet verified and is not exposed.

## SDK and host integration

The optional service stages at most 65504 bytes of PCM in a 64 KiB aligned
`.large_buffer`, appending aligned silent tail samples. A deterministic
integer triangle tone fades over 10 ms at each end. Volume is 0..100%, default
40; the audible acceptance used 60. Mute retains the chosen volume and affects
active playback; volume updates while idle leave the amplifier muted.

WAV parsing accepts RIFF/WAVE PCM format 1, one channel, 16-bit samples and
8/16/32 kHz, checking block alignment/byte rate, chunk boundaries and file
length. Unknown chunks are skipped within bounds; at most 128 chunks are
accepted. Compressed, stereo, unsupported rates, oversized or truncated input
returns an explicit error. Reads are bounded and the file is closed before
starting DAC DMA. The service never writes a WAV/card file.

APXD v1 keeps its 24-byte header and 4096-byte limit. Operation **9** commands:

| Command | Request |
| --- | --- |
| 0 | Empty; JSON info including state, rate, gain/mute, counter/ticks/errors |
| 1 | `u32 frequency, milliseconds, rate`; start tone |
| 2 | NUL-terminated UTF-8 card path; read/play WAV |
| 3 | Empty; stop |
| 4 | `u32 percent`; volume |
| 5 | `u32 boolean`; mute |
| 6 | `u32 offset` + PCM bytes; contiguous staged upload, starting at zero |
| 7 | `u32 rate`; play staged PCM |

Uploads are refused during playback/draining. Errors use `300+audio_error`,
with storage errors retaining `100+FResult`. Host `AudioClient`/CLI stages
chunks of at most 4092 PCM bytes and waits for completion. Log retrieval remains
separate. Maintenance services the optional audio poll hook without adding
automatic feeds in manual watchdog mode. BootROM/power handoffs stop audio,
mute/disable the DAC and restore owned clock/analog fields after the ACK.

The `audio-demo` retains independent JPEG/display, SD, GPIO and power features.
Audio text/constants are 8048/353 bytes in extra DDR; PCM workspace is 65536
bytes. Primary runtime uses 121616/131072 bytes, including a 16 KiB heap; stack
is a separate 4 KiB. Total RAM reservations remain 2166784 bytes, unchanged
from `jpeg-demo`. `audio-footprint.json` and the manifest record addresses,
segment hashes and pinned compiler/libc provenance.

## Acceptance

Accepted build `37f27e3d3ef105c2`, ELF SHA-256
`fa0154b439864aad92c1de928862084c55404d6fb5e8e17917f11676eb69ff23`.
The user confirmed alternating 440/660 Hz tones at 60% were audible and clean.
`audio-live.json` records all three rates, PCM sizes 2..65504 bytes including
4092/4094/4096 boundaries, byte-for-byte staged data and silent-tail readback,
WAV playback, malformed/unsupported input rejection, busy protection, volume,
mute/unmute and stop/restart. The combined run completed 43 audio jobs and
43 2048×1536 JPEG previews in 60.64 seconds, plus 176128 verified SD bytes.
Audio/display/SD/ADC error counters stayed zero; the stack-bottom 512 bytes
remained zero. GPIO/watchdog/USB status continued to work.

`audio-recovery.json` records three acknowledged BootROM/relaunch cycles while
speaker DMA was active, each with loader INQUIRY/RAM checks and subsequent
successful audio/native JPEG playback. `audio-power-off.json` records the
active-audio shutdown ACK/disconnect. The full SPI readback is recorded
separately in `audio-spi-preservation.json`; bootstrap SPI callbacks run from
a fresh stock entry, outside same-image recovery tests. Changing code layouts
in a warm loader remains a platform limitation requiring reset.

The final restored build also encoded a 2048×1536 JPEG while audio ran,
saved it to SD, passed a full Pillow decode and played it back through the
hardware decoder. `audio-final-health.json` records this regression and the
final native display/audio-ready state. The first encode request exceeded the
one-second readiness-probe deadline; its successful ACK and completed jobs
were recovered without reset. The host now restores the normal five-second
request deadline after a successful SDK handshake. A second largest-image
encode passed with that returned connection; sample preparation took 1.025 s.

Host tests pass (145 tests, six subtests). Audio/system/JPEG/display/SD/FatFs/
GPIO sanitizer models pass. The ELF audit confines 1045 direct calls to SDK
executable sections and verifies 4395 inert branch slots; reviewed indirect
calls remain SDK callbacks and the intentional BootROM handoff. See
`audio-acceptance.json` for the combined record. Temporary probes are research
history, not production interfaces.
