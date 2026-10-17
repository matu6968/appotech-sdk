# JT5295 watchdog and board shutdown

The SDK now owns watchdog feeding, supports automatic/application-managed
operation and disable, and provides graceful hardware reboot and power-off.
The GPIO demo also recognizes a two-second Power hold. Hardware access is
independent; the only intentional ROM call remains the existing BootROM handoff.

## Raw evidence and hardware probes

Main SPI offset `0x2200` maps to `0x02000000`. Raw native little-endian,
no-delay instructions take precedence over generic OpenRISC decompilation.
`watchdog-power-vendor-raw.disasm` preserves the relevant stock code.
`TraceWatchdogPower.java` records the Ghidra inventory; function analysis is
incomplete and direct-call references were mostly absent, so raw instructions
were used. `AnnotateWatchdogPower.java` adds reproducible labels/comments.
The loaded image/BootROM hashes remain in [the evidence ledger](evidence.md).

| Vendor address | Verified sequence | SDK interpretation |
| --- | --- | --- |
| `020237dc` | Argument zero writes `880c=5500`; nonzero writes `880c=50006` | Watchdog disable; keyed reload/enable with selector 6 |
| `02023824` | Test `880c` bit 4; if enabled write `50006` | Conditional stock feed |
| `02023858` | Write `880c=39` | Fast watchdog reset |
| ROM `001038c0/00103900` | Repeated `880c=5001d` | Loader feed/configuration, selector 5 |
| `02025f44/02025f94` | Write byte at `88ec`; poll `88e0` bit 3 | Always-powered serial bus transmit/receive |
| `02026130/020261b8` | `88e0` bit 8 selects transaction; command then byte(s); deselect | Even read / odd write command addresses |
| `0200ad7c..0200ad94` | Clear PM54 bit 5 through command 55; PM45=10; PM43=e0; PM41=02 | Board shutdown sequence |

The controlled calibration image replaced the old fixed maintenance write with
a service-word mailbox. Its ELF/manifest/segments and the small mailbox source
are retained under `archive/watchdog-probe.*` and `archive/watchdog-probe-*.c.txt`.
`watchdog-first-control.json` records that `5500` reads back zero, `50006` reads
back `1e`, and `5001d` reads back `1d`. Thus the key reloads/enables the watchdog
and reset path (readback bits 4/3), with the low three bits selecting its period.
Programming the disable key remains effective when polling continues.

Stopping all feeds while continuing USB requests caused an actual hardware
reset and a new stock MSC device on physical port `7-1`.

| Selector | USB-observed expiry |
| --- | --- |
| 4 | 0.4594 s |
| 5 | 1.8166 s |
| 6 | 7.3066 s |
| 7 | 29.1030 s |

See `watchdog-first-expiry.json` and `watchdog-period-calibration.json`.
Measurements include host/USB observation latency; they are not oscilloscope
measurements. The factor-of-four progression is verified for 4–7. Encodings
0–3 are accepted but their shorter periods have not been physically timed.
The approximate RC clock source is inferred; CPU-clock changes were not part
of this timing test. Hardware expiry does not depend on the SDK's software tick
accumulator. Reset-cause reporting is not claimed: BootROM's
own watchdog initialization has already run before SDK takeover.

`power-domain-first-read.json` records the inherited bus control `00220245`
and read-only PM-bank snapshot; transaction cleanup restored control exactly.
The PM byte transfer uses a bounded 5 ms deadline and releases chip select
on error. It rejects an already-selected bus transaction. Public SDK reads
are limited to even addresses `40..5a`; writes remain private to shutdown.
Exact power-rail and charging meanings of these fields remain undocumented.

## Runtime and public APIs

`apx_maintenance` is now an external policy-aware function. Runtime initializes
automatic selector 5, maintaining the behavior of existing SDK examples.
`apx_watchdog_start(selector, mode)` validates 0–7 and automatic/manual mode
before touching the controller. `apx_watchdog_feed`, `stop` and `get_info` expose
the remaining operations. Automatic mode feeds in waits/polling. Manual mode
never feeds from USB, display, SD, formatting or other maintenance calls;
only the application's explicit heartbeat or APXD feed reloads it. Statistics
are unsigned counters and can wrap. `apx_watchdog_reset` is a low-level fast
reset without storage synchronization.

`apx_reboot` and `apx_power_off` prepare through the existing storage lifecycle
hook: sync/unmount healthy media and reject open file/DIR handles. Preparation
returns an APXD status (`0x100+FRESULT` for storage errors) before teardown.
The two-phase prepare/commit API lets APXD acknowledge first. After transmission
drains, commit disables the watchdog, permits the final bulk URB to finish,
stops inputs/display/SD DMA, detaches USB and masks external interrupts.
Reboot then requests the fast hardware watchdog reset. Power-off applies the
stock PM sequence and stops running normal SDK work. Bus failures stop that
sequence; physical reset may be needed after an interrupted shutdown.

The existing BootROM return now disables an application watchdog before its
acknowledged teardown delay, allowing the ROM to install its own policy.
There is no vendor callback cleanup against a running SDK. Hardware-reset
tests confirm a new stock MSC identity before entering BootROM and releasing
the prior SDK's exact reservations.

APXD SYSTEM operation 7 adds INFO, WATCHDOG_START, FEED, STOP, REBOOT and
POWER_OFF without changing the 24-byte v1 header. Invalid length, mode,
selector, width or address space is rejected. Reboot/off are deferred until
the ACK is transmitted. Host commands report acknowledgement; test tools
separately observe the physical-port transition and validate file persistence.
The [SDK README](../README.md) records exact frames, CLI commands and C APIs.

In `gpio-demo`, a short Power press still updates the tile. Two seconds of
debounced high state requests graceful power-off. A lock/sync error keeps the
SDK running, logs the error and retries only after another two seconds of hold.
Library users choose their own button policy.

## Verification and reservations

`watchdog-power-first-live.json` verifies default/disabled/manual/automatic
policies with USB, GPIO and real SD operations. Feed counts remain unchanged
through unrelated requests in manual mode. SD write/readback passes with zero
controller errors. `power-off-first-live.json` records ACK and USB disconnect
after about 100 ms; the owner confirmed a normal Power-button boot afterward.
`watchdog-force-reboot-live.json` independently verifies the test file survived
power-off and the graceful reset returned a stock `Buildwin Media-Player`
INQUIRY. These are USB-connected board tests; battery-only standby current,
rail voltages and charger behavior have not been measured.

`test_watchdog_live.py` performs sustained SD write/readback in automatic and
explicit-feed modes, verifies malformed requests have no configuration effect,
checks display/GPIO/stack state and remount persistence, and restores automatic
selector 5. It only writes the dedicated `SDKWDT.BIN` file on the authorized
disposable card. Subsequent live/cycle reports and final SPI preservation are
listed in `watchdog-power-acceptance.json`.

The final `b7a7e309b4559674` GPIO image verified 1,050,624 bytes over 30.09 s in
automatic mode and 1,034,240 bytes over 30.06 s with explicit manual feeds.
Manual feed counts changed only for the requested heartbeat, and SD/ADC/display
timeouts remained zero. `power-button-live.json` records a first 1.83-second
press which did not shut down, a later hold which did, a verified ten-second
off interval and normal MSC restart. The owner confirmed off/on looked normal.
`watchdog-final-expiry.json` records reset after about 0.457 s with manual
selector 4 while USB requests continued, and verifies the test file survived
the Power-button shutdown. The relocated framebuffer's final live report is
`watchdog-power-display-final-live.json` for build `dee02ef05bb6ecc1`.

The sanitizer model executes the actual watchdog, power and APXD implementations.
It checks disabled/manual feeding, invalid configuration, PM bounds/busy/timeouts,
storage-lock refusal, command framing and the ordering of ACK, watchdog stop,
peripheral teardown, reset and shutdown. SD, FatFs, display and GPIO regression
models remain in use, alongside the host suite.

Adding system support initially overflowed the full-frame display demo's
128 KiB runtime by 2,488 bytes. Its 77,440-byte RGB565 framebuffer now uses
`.large_buffer` in the existing `02220000..022dffff` extra DDR bank. Unicode
tables remain first in that bank when present; large buffers follow at 32-byte
alignment. Manifest generation includes the extra reservation when either is
used, startup relies on verified zero preflight for these NOLOAD buffers, and
APXD protects the entire bank. No RAM reservation shape, stack size or 16 KiB
heap default changed. Final display tests cover RGB565 DMA and native NV21
with the moved source, then acknowledged BootROM release of all four regions.

Final linked sections, toolchain/segment hashes, native results, same-image
recovery cycles, complete SPI comparison and the device state left running
are recorded in `watchdog-power-builds.json` and `watchdog-power-acceptance.json`.
