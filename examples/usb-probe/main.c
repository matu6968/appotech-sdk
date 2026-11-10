/* Phase-0 probe of the JT5295 USB core (see research/usb-musb.md).
 *
 * Answers, on hardware, what a TinyUSB device driver needs to know:
 *   - how many endpoints exist (which INDEX values have writable TXMAXP/RXMAXP),
 *   - whether the FIFOs are static or dynamically sized (TXFIFOSZ/RXFIFOSZ/ADD at 0x62..0x67),
 *   - what the MUSB identification registers (EPINFO 0x78, RAMINFO 0x79, CONFIGDATA at INDEX 0 offset 0x1f) report.
 *
 * The probe runs with the pull-up off (the USB core is brought up by apx_usb_init(), then SOFTCONN is dropped),
 * restores every register it writes, and then calls apx_usb_init() again so the APXD link comes back normally.
 * It never touches the FIFO windows (0x20..0x5f) or the read-to-clear interrupt registers (0x02..0x05, 0x0a).
 *
 * The raw per-index dump is kept in `usb_probe_dump` (16 indexes x 128 byte registers, offset order) so a host can
 * read it with `apxtool` memory reads even if the log ring has wrapped; the summary is printed to the log/UART.
 */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <stdint.h>
#include <stdio.h>

#define USB_SPR(offset) (0xa000u + 4u * (unsigned)(offset))
#define POWER 0x01
#define INDEX 0x0e
#define TXMAXP 0x10
#define RXMAXP 0x14
#define FIFOSZ_TX 0x62
#define FIFOSZ_RX 0x63
#define FIFOADD_TX 0x64
#define FIFOADD_RX 0x66
#define EPINFO 0x78
#define RAMINFO 0x79
#define CONFIGDATA 0x1f /* at INDEX 0 */

uint8_t usb_probe_dump[16][128] __attribute__((aligned(4)));
volatile uint32_t usb_probe_done;

static unsigned rd(unsigned offset) { return apx_spr_read(USB_SPR(offset)) & 0xffu; }
static void wr(unsigned offset, unsigned value) { apx_spr_write(USB_SPR(offset), value & 0xffu); }
static unsigned rd16(unsigned offset) { return rd(offset) | rd(offset + 1u) << 8; }
static void wr16(unsigned offset, unsigned value) { wr(offset, value); wr(offset + 1u, value >> 8); }

/* Registers that clear on read or pop a FIFO: never touched by the dump. */
static int skip(unsigned offset) {
    return (offset >= 0x02 && offset <= 0x05) || offset == 0x0a || (offset >= 0x20 && offset <= 0x5f);
}

static void dump_index(unsigned index) {
    wr(INDEX, index);
    for (unsigned offset = 0; offset < 128; ++offset) usb_probe_dump[index][offset] = skip(offset) ? 0xee : (uint8_t)rd(offset);
}

/* Write a pattern into a 16-bit MAXP register and report which bits stick; the original value is restored. */
static unsigned maxp_mask(unsigned offset) {
    unsigned original = rd16(offset);
    wr16(offset, 0xffffu);
    unsigned mask = rd16(offset);
    wr16(offset, original);
    return mask;
}

/* Same for a byte register (dynamic FIFO size/address). */
static unsigned byte_mask(unsigned offset) {
    unsigned original = rd(offset);
    wr(offset, 0xffu);
    unsigned mask = rd(offset);
    wr(offset, original);
    return mask;
}

static void probe(void) {
    unsigned power = rd(POWER);
    wr(POWER, power & ~0x40u); /* SOFTCONN off: the host sees us disconnect while we probe */
    apx_delay_ms(50);
    printf("usb-probe: POWER=%02x DEVCTL=%02x EPINFO=%02x RAMINFO=%02x FS=%02x%02x\n", rd(POWER), rd(0x60), rd(EPINFO), rd(RAMINFO), rd(0x7a), rd(0x7b));
    wr(INDEX, 0);
    printf("usb-probe: CONFIGDATA(idx0,0x1f)=%02x INDEX readback=%u\n", rd(CONFIGDATA), rd(INDEX));
    for (unsigned index = 0; index < 16; ++index) {
        dump_index(index);
        wr(INDEX, index);
        unsigned readback = rd(INDEX);
        if (index == 0) {
            printf("usb-probe: ep0 idx=%u csr0=%02x count0=%02x fifosz/cfg=%02x\n", readback, rd(0x12), rd(0x18), rd(CONFIGDATA));
            continue;
        }
        unsigned txm = maxp_mask(TXMAXP), rxm = maxp_mask(RXMAXP);
        unsigned txsz = byte_mask(FIFOSZ_TX), rxsz = byte_mask(FIFOSZ_RX);
        unsigned txadd = byte_mask(FIFOADD_TX), rxadd = byte_mask(FIFOADD_RX);
        printf("usb-probe: ep%-2u idx=%u maxp mask tx=%04x rx=%04x | fifosz tx=%02x rx=%02x add tx=%02x rx=%02x | ep fifosz=%02x csrh tx=%02x rx=%02x\n",
               index, readback, txm, rxm, txsz, rxsz, txadd, rxadd, rd(CONFIGDATA), rd(0x13), rd(0x17));
    }
    wr(INDEX, 0);
    wr(POWER, power);
    usb_probe_done = 0x50524f42u; /* "PROB" */
    printf("usb-probe: done; raw dump at %p (16 x 128 bytes)\n", (void *)usb_probe_dump);
}

int main(void) {
    printf("usb-probe starting\n");
    apx_usb_init();
    probe();
    apx_usb_init(); /* bring the debug link back from a clean state */
    for (;;) apx_debug_poll();
}
