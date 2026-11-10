/* Probe of the USB core's endpoints 2..4 and of the packet RAM beyond EP1's buffers. Build with EXAMPLE_DEFS=-DAPX_USB_PROBE_EPS=1: that adds a second
 * interface with bulk IN endpoints 0x82..0x86 to the debug device (src/tusb/usb_tusb.c). tools/probe_usb_ep.py reads them.
 *
 * Stock sends on EP2..4 with: SPR 0x9858 / 0x9860 / 0x9868 = a packet RAM address, SPR 0x9844 = length | 1 << (13 + ep), TxPktRdy (stock 0x202e808).
 * This does the same from a pattern it stores in the packet RAM and logs what happens. It also checks how far the packet RAM extends (write, read back,
 * restore; word accesses from 0x8800 up to 0xa000).
 */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <stdint.h>
#include <stdio.h>

#define USB(o) (0xa000u + 4u * (unsigned)(o))
enum { INDEX = 0x0e, TXCSRL = 0x12, TXCSRH = 0x13, TXMAXP = 0x10 };
enum { TX_RDY = 0x01, TX_FLUSH = 0x08 };
#define VENDOR_CTRL 0x9844u
static unsigned rd(unsigned o) { return apx_spr_read(USB(o)) & 255u; }
static void wr(unsigned o, unsigned v) { apx_spr_write(USB(o), v & 255u); }

static void extent(void) {
    enum { FIRST = 0x8800, LAST = 0xa000, STEP = 0x400, N = (LAST - FIRST) / STEP };
    uint32_t saved[N];
    for (unsigned i = 0; i < N; ++i) saved[i] = *(volatile uint32_t *)(uintptr_t)(FIRST + i * STEP);
    for (unsigned i = 0; i < N; ++i) *(volatile uint32_t *)(uintptr_t)(FIRST + i * STEP) = 0xa5a50000u | (i * STEP >> 4);
    unsigned ok = 0;
    for (unsigned i = 0; i < N; ++i) {
        uint32_t got = *(volatile uint32_t *)(uintptr_t)(FIRST + i * STEP);
        printf("ram %04x: %s (%08x)\n", FIRST + i * STEP, got == (0xa5a50000u | (i * STEP >> 4)) ? "ok" : "no", (unsigned)got);
        ok += got == (0xa5a50000u | (i * STEP >> 4));
    }
    for (unsigned i = 0; i < N; ++i) *(volatile uint32_t *)(uintptr_t)(FIRST + i * STEP) = saved[i];
    printf("ram extent: %u of %u probe words held their value\n", ok, (unsigned)N);
}

static void send(unsigned ep, unsigned len, unsigned address, unsigned variant) {
    volatile uint32_t *ram = (volatile uint32_t *)(uintptr_t)address;
    for (unsigned w = 0; w < 128; ++w) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4; ++b) v |= (uint32_t)(uint8_t)((w * 4 + b) * (ep + 3) + ep) << (8 * b);
        ram[w] = v;
    }
    const unsigned address_spr = 0x9850u + 8u * (ep - 1u);
    wr(INDEX, ep);
    const uint32_t a0 = apx_spr_read(address_spr), c0 = apx_spr_read(VENDOR_CTRL);
    const unsigned maxp = rd(TXMAXP) | rd(TXMAXP + 1) << 8;
    wr(TXCSRH, rd(TXCSRH) | 0x20);
    if (variant == 0) apx_spr_write(address_spr, address);
    apx_spr_write(VENDOR_CTRL, len | 1u << (13 + ep));
    uint32_t t0 = apx_ticks();
    wr(TXCSRL, TX_RDY);
    const unsigned csr_now = rd(TXCSRL);
    unsigned timeout = 0;
    while (rd(TXCSRL) & TX_RDY) { apx_maintenance(); if (apx_ticks() - t0 > 32768u * 2) { timeout = 1; break; } }
    printf("ep%u len %u variant %u @%04x: maxp %u spr%04x %08x->%08x ctrl %08x csr %02x/%02x %s\n", ep, len, variant, address, maxp, address_spr,
           (unsigned)a0, (unsigned)apx_spr_read(address_spr), (unsigned)c0, csr_now, rd(TXCSRL), timeout ? "TIMEOUT" : "sent");
    if (timeout) { wr(TXCSRL, TX_FLUSH); wr(TXCSRL, TX_FLUSH); }
    apx_spr_write(address_spr, a0);
    wr(TXCSRH, rd(TXCSRH) & ~0x20u);
    wr(INDEX, 0);
}

int main(void) {
    printf("usb-ep-probe starting\n");
    apx_usb_init();
    uint32_t until = apx_ticks() + 32768u * 12;
    while (!apx_deadline_expired(until)) apx_debug_poll();
    printf("vendor: 9844 %08x 984c %08x 9850 %08x 9854 %08x 9858 %08x 985c %08x 9860 %08x 9864 %08x 9868 %08x 986c %08x\n", (unsigned)apx_spr_read(0x9844),
           (unsigned)apx_spr_read(0x984c), (unsigned)apx_spr_read(0x9850), (unsigned)apx_spr_read(0x9854), (unsigned)apx_spr_read(0x9858),
           (unsigned)apx_spr_read(0x985c), (unsigned)apx_spr_read(0x9860), (unsigned)apx_spr_read(0x9864), (unsigned)apx_spr_read(0x9868), (unsigned)apx_spr_read(0x986c));
    extent();
    static const unsigned plan[][3] = { /* ep, length, variant (0 = set the address register, 1 = leave it) */
        {2, 512, 0}, {3, 512, 0}, {4, 512, 0}, {2, 64, 0}, {3, 100, 0}, {4, 200, 0}, {2, 512, 1}, {3, 512, 1}, {4, 512, 1} };
    for (unsigned i = 0; i < sizeof plan / sizeof plan[0]; ++i) {
        send(plan[i][0], plan[i][1], 0x8800 + 0x200 * (plan[i][0] - 2), plan[i][2]);
        uint32_t wait = apx_ticks() + 32768u / 2;
        while (!apx_deadline_expired(wait)) apx_maintenance();
    }
    printf("usb-ep-probe done\n");
    for (;;) apx_debug_poll();
}
