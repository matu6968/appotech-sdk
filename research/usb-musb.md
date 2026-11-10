# JT5295 USB core: hardware probe (Phase 0 of the TinyUSB port)

Source: `examples/usb-probe` RAM-launched on an Action Vision camera, 2026-11-08 (build 33bc75928fc4363f).
"Measured" means read back from hardware; "Inferred" is interpretation and has not been tested.

How to repeat:

    make EXAMPLE=usb-probe
    bash ../Action_Digital_Kids_Camera_DOOM/tools/launch.sh build/usb-probe.manifest.json
    PYTHONPATH=../apxtool/src:../jl-uboot-tool python3 -m apxtool debug --port <usb port> logs
    ... debug --port <port> read --output dump.bin <address printed in the log> 2048   # raw 16 x 128 byte dump

## Measured

| Item | Value |
| --- | --- |
| POWER (pull-up off) | `0x20` (HSENAB) |
| DEVCTL (`0x60`) | `0x99` |
| EPINFO (`0x78`) / RAMINFO (`0x79`) | `0x00` / `0x00` (the standard MUSB ID registers are not implemented) |
| CONFIGDATA (INDEX 0, `0x1f`) | `0x1b` |
| Endpoints with writable TXMAXP and RXMAXP | INDEX 1..6 (both directions). INDEX 7..15 read back 0 |
| Writable MAXP bits | `0x1fff` on every endpoint (13 bits) |
| Offsets `0x62..0x67` | the same value at every INDEX (tx `0x10`/rx `0xfc` size masks, tx `0xff`/rx `0x20` address masks), so they are **not** indexed dynamic-FIFO registers |
| Offset `0x1f` at INDEX 1,2,3,5,6 / INDEX 4 | `0xaa` / `0xcc` (mirrored at `0x6f`); offset `0x6c` reads `0xbe` at every INDEX |
| Vendor SPR `0x984c` / `0x9850` / `0x9854` | `0x8000` / `0x8040` / `0x8240` (`0x9858..0x986c` are 0); `0x9840`=`0x300170c5`, `0x9844`=`0xd`, `0x9870`=`0x80000007` |

## Inferred

- CONFIGDATA `0x1b` decodes with the standard MUSB bit meanings as: 16-bit UTMI, SoftConnect present, **DynFIFOSizing = 0 (static FIFOs)**, high-bandwidth TX and RX endpoints, little-endian FIFO, no bulk splitting or combining. This fits a Mentor MUSB core.
- 13 MAXP bits (11 size bits plus 2 multiplier bits) match high-bandwidth isochronous support up to 3 packets per microframe. Isochronous UVC is therefore plausible. Not tested.
- Six endpoint pairs exist, so `TUP_DCD_ENDPOINT_MAX` is 7 (EP0 + 6). The stock descriptor table that suggested EP 0x81..0x87 and 0x91..0x97 did not match the hardware: no EP7 exists.
- FIFO placement looks vendor-configured through SPR `0x984c/0x9850/0x9854` (bit 15 = enable, low bits = a start address): EP0 at 0, then a block from `0x40` to `0x240`, i.e. 512 bytes, which is what the SDK uses for EP1. Not verified.
- The `0x1f` per-endpoint values do not decode to plausible FIFO sizes with the standard `2^(n+3)` nibble formula (it would give 8 KiB and 32 KiB), so do not trust them as sizes.

## Consequences for the TinyUSB port

- Use `MUSB_CFG_DYNAMIC_FIFO 0` in the port header. The stock `dcd_musb.c` dynamic-FIFO path would write `0x62..0x67`, which are not FIFO registers here. Do not write those offsets.
- Whether TX and RX of one endpoint share a FIFO (`MUSB_CFG_SHARED_FIFO`) is still unknown.
- Unknown and needed before MSC/UVC with more than EP1: the real FIFO size of EP2..EP6, and how `0x984c..0x9854` map to them (probe v2: change one of these registers, then write a packet into the EP2 TX FIFO window at SPR `0xa000 + 4*(0x20 + 4*ep)` and read back how many bytes fit before TXCSR reports a full FIFO).
- The probe wrote `0xff` to offsets `0x62..0x67` and restored them. The debug link recovered normally, but a v2 probe should leave those alone.
- Not yet checked: whether INTRUSB/INTRTX/INTRRX read-to-clear (the host model assumes it), and the USB interrupt source.

## Packet RAM and the vendor "DMA" registers (2026-11-08, `examples/usb-dma-probe` + `tools/probe_usb_dma.py`, Silvergear RAM launch)

Question: does the core have a DMA engine that moves packets between DDR and the endpoint FIFOs? The stock firmware kicks EP0/EP1 transfers by writing SPR `0x984c` (EP0) /
`0x9850` (EP1 IN) /`0x9854` (EP1 OUT) with an address and SPR `0x9844` with `length | 1 << (13 + endpoint)`, then sets TxPktRdy, which looks like a DMA descriptor.

**Answer: no DMA into DDR was found; the FIFO RAM itself is memory mapped, which is almost as good.** Measured:

| Item | Result |
| --- | --- |
| Writing a DDR address to `0x9850`, then `0x9844 = len \| 1<<14` and TxPktRdy | The register reads back the address, but the packet sent has the requested **length** and the **old contents of the packet RAM**, not the DDR buffer |
| Packet RAM | Memory mapped from `0x8000` (the EP0 SETUP packet is at `0x8000`). Uncached; word loads and stores work |
| EP1 IN buffers | `0x8040` and `0x8240`, 512 bytes each, used alternately (ping-pong); `0x9844` bit 20 says which one the next packet goes to (0 = `0x8040`) |
| EP1 OUT buffers | `0x8240` and `0x8440`, alternately; `0x9844` bit 26 says which one holds the packet being read (0 = `0x8240`). The IN pair and the OUT pair overlap at `0x8240` (shared FIFO), harmless while the protocol alternates directions |
| Sending a packet | store the data into the IN buffer, write `0x9844 = length \| 1 << (13 + ep)` (the start bit is what latches the length; without it a zero-length packet goes out), set TxPktRdy. TxPktRdy clears at once and FIFONE shows the packet waiting: the IN side is double buffered |
| Zero-length packets | `0x9844 = 1 << 14` plus TxPktRdy sends one (the APXD tests with exact multiples of 512 pass) |
| Cost of 512 bytes | byte-wise FIFO SPR write 49 us; 32-bit stores into the packet RAM 8.0 us; 32-bit loads 10.6 us (a byte-wise FIFO read is about 70 us at 150 ns per SPR access) |
| Vendor registers at start-up | `0x984c` `0x8000`, `0x9850` `0x8040`, `0x9854` `0x8240` (BootROM defaults, EP0 / EP1 IN / EP1 OUT-ish buffer addresses); the stock transfer routines rewrite them per transfer, so they are buffer-address registers inside the packet RAM, not DDR pointers (they read back whatever is written, a DDR address included) |

EP2..EP6 packet RAM is **not** characterised; the SDK driver uses the packet RAM only for EP1 and keeps the FIFO SPR for the rest.
The Action camera was not connected for this probe. Its core is the same and the packet RAM path was verified there later (2026-11-09/10, research/action-vision-compat.md); TinyUSB is the default stack on both boards.

## Bulk IN throughput: where 4.5 MB/s went (2026-11-09, `examples/uvc-bench`)

The video path was stuck at 2.5 MB/s in the camera and 4.05-4.5 MB/s in a bare bench, and looked like a USB limit. It was not: **the core and the host can do about 44 MB/s**
(a raw loop that refills EP2 with no copy and no TinyUSB sent 88,000 512-byte packets per second; with the packet RAM copy of a warm buffer 85,000, with the whole
`apx_usb_poll()` in the loop 68,000, with both 37,000). What limited the real path was the CPU:

- **TinyUSB's video class copies every payload** (UVC header + data) into its payload buffer before sending. On this CPU a 2 KB copy takes 227 us when the source is cold
  (JPEG data the encoder has just written; the cache is invalidated for it), about 9 MB/s: a DDR line fill costs roughly **0.55 us per 16-byte line** (64 KB sequential cold read:
  28 MB/s; the same 2 KB warm: 39-42 us). The copy also allocates and later writes back the destination's lines, which is where the rest goes when buffers conflict in the small cache.
  Per-payload cost was therefore about 450 us for 2 KB = 4.5 MB/s, independent of frame size (20/60/120 KB frames all gave the same byte rate) and of the payload size (16 KB payloads
  gave the same rate).
- **Sending from the application's buffer instead** (the UVC header written in place before the data, the DCD reading the packets from there) sustained 11.6 MB/s in the bench
  (100 KB frames at 116 per second, every frame byte-exact) and takes 2.2 ms for a 15 KB frame in the camera. The raw loop with cold-source reads straight from DDR measured 24 MB/s
  (13 MB/s with the full poll), so there is more room; nothing needs it yet.
- **TxPktRdy clears right after the write** (the first CSR read after setting it shows RDY clear, FIFO not empty): the core accepts a second packet at once into the other buffer. The
  DCD loads two packets per refill now (`try_write`), which was worth 4.05 -> 4.5 MB/s by itself, and completion waits for the FIFO to drain before the FIFO mode is flipped. **Both apply to EP2 and up only:** EP1's FIFO is shared with OUT, and waiting there deadlocks when the host's next request has already landed in it (found on the Action camera, research/action-vision-compat.md).
  The host does NAK-and-retry at microframe granularity, so a single packet at a time caps a bulk IN stream near 8000 packets (4 MB/s) per second; with two queued it does not.
- `pram_store` reads an unaligned source with aligned loads shifted together (one load per word); the old byte-by-byte path was four times the loads.

## Endpoints 2..4 and the packet RAM map (2026-11-08, `examples/usb-ep-probe` + `tools/probe_usb_ep.py`, built with `EXAMPLE_DEFS=-DAPX_USB_PROBE_EPS=1`)

- The packet RAM is **4 KiB, `0x8000..0x8fff`**: words written at `0x8800` and `0x8c00` read back, `0x9000` and above read zero and hold nothing. Isochronous 3 x 1024-byte
  packets (stock's EP4, wMaxPacketSize 0x1400) therefore cannot be double buffered here; the SDK's video path is bulk.
- **EP2, EP3 and EP4 transmit exactly like EP1**, with a bulk IN endpoint of 512 bytes: store the packet in RAM, write the endpoint's address register, write
  `SPR 0x9844 = length | 1 << (13 + ep)`, set TxPktRdy. Address registers: `0x9850` (EP1 IN), `0x9858` (EP2), `0x9860` (EP3), `0x9868` (EP4); EP1 OUT is `0x9854`
  (stride 8: the odd words are presumably the OUT side of each endpoint, not tried). They are **0 after BootROM except EP1's** (`0x9850 = 0x8040`, `0x9854 = 0x8240`),
  so every other endpoint needs its register written before use. Written with the address of a DDR buffer they hold it and send RAM contents anyway (no DMA).
- The register gives the start of a **pair of 512-byte buffers used alternately**: the second packet is read from `address + 0x200`. The toggle that says which buffer
  the next packet goes to is `SPR 0x9844` bit `19 + ep` for IN (bit 20 EP1, 21 EP2, 22 EP3, 23 EP4; EP1 OUT is bit 26). A send that does not write the address register
  sends whatever the RAM holds at the default (a stale descriptor in the test).
- Map used by the SDK driver (as of the UAC microphone, 2026-11-09): `0x8000` EP0 (64) | EP1 IN and OUT both at `0x8040/0x8240` (`0x9854` is written with `0x8040`; the BootROM default put OUT at
  `0x8240/0x8440`, which the iso endpoint needs) | `0x8800/0x8a00` EP2 IN (video 0) **and** EP3 IN (video 1): the two share their buffers, so a packet of one must be out before the other writes
  (`shared_busy()` in dcd_apx.c waits up to 3 ms, then flushes the stale packet; the example sends one video stream at a time) | EP4 IN iso (the microphone) at `0x8c40`. EP5, EP6 and any OUT endpoint above EP1 are untested.
- **The second buffer of an iso IN endpoint is not at +0x200.** For EP4 the alternate packet is read from `address - 0x800` (found with a marker experiment: every 64-byte unit of the RAM above EP1 was filled
  with its own address, and the bad packets on the wire showed which unit was being read). With the base at `0x8c40` the pair is `0x8c40/0x8440`, which is why EP1 had to move down to `0x8040/0x8240` and
  EP2/EP3 to share. Bulk EP1..EP3 keep the +0x200 stride. Whether the bulk stride also changes with TXMAXP was not established.
- Isochronous IN as the SDK drives it: `TXMAXP = wMaxPacketSize`, `TXCSRH = ISO (bit 6) | MODE_TX (bit 5)`, address register written, flush (clears the toggle), `INTRTXE` bit set; packets are queued like bulk ones. A packet
  larger than TXMAXP makes the host see babble/EIO, so a driver must never exceed it.
- Stock's configurations (read from the Silvergear stock image, `stock-app-02000000.bin` at `0x85c44..0x862d3`): device class EF/02/01, bcdUSB 1.10, VID:PID 1908:3283/3282; four
  configuration variants, the full one = IAD(video control IF0 + streaming IF1) + IAD(audio control IF2 + microphone IF3) + mass storage IF4. IF0 has an interrupt IN status endpoint
  EP2 (64), IF1 has MJPEG (format subtype 6) with up to five frames (640x480 default among them) and alt settings 1/2 with **isochronous IN EP4, wMaxPacketSize 0x1400**
  (3 x 1024 per microframe), IF3 an isochronous 16-bit mono microphone on EP3 (64 B, alt 5), IF4 bulk 512 on EP1.
