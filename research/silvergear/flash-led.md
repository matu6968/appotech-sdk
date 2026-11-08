# Silvergear flash LED (stock trace, 2026-11-07)

Status: **found by static trace of the stock app and confirmed on hardware (2026-11-07)**: the white LED lit when PB0
was driven as a push-pull output with the latch high (`spr-write 0x8044 0x7e`, `0x8004 0x23`). The Power-click toggle in
`printer-camera-demo` (build `ab2437414b9aa886`, RAM-only launch) was confirmed by the owner: the LED toggles on each short click.
The flash LED is **PB0 (bank B, mask 0x0001), active high**, a plain push-pull GPIO output.

Image: `stock-app-02000000.bin` (sha256 `9f9b9e62...`), addresses are RAM. The JT5295 core has **no branch delay slots**
(the SDK builds with `-fno-delayed-branch`, `startup.S` uses `.nodelay`, and the Ghidra `JT5295` language links calls at PC+4): an
instruction listed after a `l.jal`/`l.j` runs after the return, and a call's arguments are set up in the instructions just
*before* it (for example `addi r3,..; addi r5,..; jal 2034d54; addi r3,r0,1` = args for the call, then the first setup of the next).
(Correction 2026-11-08: an earlier edit of this paragraph claimed delay slots; the stock listing and the project's own language
module show it does not.)

## Why the pad scan missed it
The black-box pad scan of 2026-11-07 (`flash-led-search-20261107.md`, archived in `cleanup-archive-20261108/dated-bringup-reports-20261102-20261108.tar.gz`) drove every free pad and PM bit, but its deny list protected PB0/PB1 as the
key ladder. The ladder's analog input is PB1 (ADC channel 3); PB0 carries nothing else in the SDK. The
"no xor-toggle site" observation was right: the toggle goes through a board-device ioctl.

## Stock chain
1. **Key code.** `FUN_02003640` converts ADC channel 3 to a raw code with the table at `0209ed8c`
   (8-byte entries, code then ladder centre): POWER 15 -> **37**, LEFT 330 -> 28, RIGHT 240 -> 29,
   MODE 620 -> 27, SHUTTER 510 -> 34, EXIT 420 -> 38, PRINT 130 -> 41. This matches the codes in
   `board.h` (28/29/27/34/38).
2. **Click detection.** `FUN_02003714` (called from `silvergear_buttons_ioctl` `02003b28` via `FUN_02003a58`)
   emits the raw code on a short press and a long code on a hold. POWER: click 37, hold (30 samples) 39.
3. **Event table.** The Video Recorder mode record (`0209fbb4`, entry `020109e8`, poll `0200ffb4`) uses the
   event table at `0209fbf8` (`{event, handler}` pairs). Event 37 -> `02011790`, event 38 (EXIT) -> `02011898`.
   Event 27 (MODE) -> `02011314` flips the front/rear camera (state byte `020a3478`), which is not the LED.
4. **Power-click handler `02011790`.** On a press event it toggles the byte `020b5905` (0/1) and calls
   `02002ce4(dev = byte 020b58ae, cmd = 0x02000011, arg = new state)`, then redraws via `020115b0`.
5. **Device dispatcher `02002ce4`.** Indexes the device table at `0209ecb8` (24-byte records) and calls the
   record's ioctl. `020b58ae` is the index returned by opening the device named at `02082e62` = **"ir"**
   (opened in the startup function at `02000264`, next to "ad-key", "sd-card", "lcd", "cmos-sensor").
6. **"ir" ioctl `0200323c`.** Command `0x02000011` stores the level in `020a4e88` and runs
   `02034d54(1,1,0,0)` then `02034f00(1,1,level)`; command `0x02000012` reads `020a4e88` back.
   The device open `02002eb4` does the same with level 0.
7. **Pad helpers** (group = bank A..E = 0..4, SPR = base + 4*group):
   `02034d54(group,mask,dir,pull)` sets 0x8380, clears 0x8100/0x8140, writes direction 0x8040 (0 = output),
   clears the force registers 0x81c0/0x8200 and the pull registers 0x8080/0x80c0, exactly like
   `apx_gpio_configure` (`src/gpio.c`). `02034f00(group,mask,level)` writes the latch 0x8000.
   Group 1 mask 1 is therefore PB0.
8. **Off paths.** Startup calls the ioctl with 0 at `02000298`; the shutdown routine `0200eb2c` does so at
   `0200eb88`.

Stock behaviour matches the owner's report: the level is held between clicks, and it is a state toggle,
not a capture pulse.

## SDK
`boards/silvergear-3222882/board.h` now sets `APX_STATUS_LED_MASK 0x0001u` (group 1), so the existing
`apx_led_set()` / `apx_led_get()` (`src/buttons.c`) drive PB0 through the force-high/force-low registers;
`apx_buttons_init()` leaves it forced low. `examples/printer-camera-demo` toggles it on a Power click
(press shorter than 1 s) and shows `FLASH ON` in the overlay. `tests/gpio_model.c` covers both boards.

The first SDK version used the Action-camera method (pad as floating input plus the force-high/low registers
81c0/8200). On hardware that did **not** light the LED (`81c4`/`8204` also read back 0 after `led on`), so on this
board `apx_led_set` now follows stock: `apx_gpio_configure(1,1,OUTPUT,...)` then `apx_gpio_write` (board.h
`APX_STATUS_LED_LATCH 1`; the latch bit is cleared again in `apx_buttons_shutdown`).

## Hardware results
- Direct stock-style register writes (output + latch high) on a RAM-only `printer-camera-demo` launch: LED lit.
- Old override-based `apx_led_set`: no light.
- Rebuilt latch-based `apx_led_set` (`debug gpio led on`): LED lit, latch `8004` bit 0 set.
- Power click in `printer-camera-demo`: LED toggles as expected (confirmed by the owner).
- Not tested: an SPI install of an image containing this change (only RAM launches were used).
