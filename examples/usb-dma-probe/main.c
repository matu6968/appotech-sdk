/* Probe for a USB data DMA engine in the JT5295 core. RESULT (2026-11-08, Silvergear): there is none into DDR. Stock kicks transfers with
 * SPR 0x9850 = a memory address and SPR 0x9844 = length | 1 << (13 + endpoint), but writing a DDR address there changes nothing: the core sends from its
 * own packet RAM, memory mapped at 0x8000. EP1 has a pair of 512-byte IN buffers at 0x8040 / 0x8240 (0x9844 bit 20 = the one the next packet uses) and a
 * pair of OUT buffers at 0x8240 / 0x8440 (0x9844 bit 26 = the one holding the received packet). A packet is sent by storing it into the IN buffer, writing
 * its length with the start bit to 0x9844 and setting TxPktRdy. 32-bit stores into that RAM take 8 us per 512 bytes (49 us through the FIFO SPR).
 * See research/usb-musb.md. The SDK driver (src/tusb/dcd_apx.c) uses this for EP1.
 *
 * Runs on top of the normal USB stack (debug mode), waits for the host, then:
 *   1. times 512-byte PIO writes into the EP1 FIFO and word accesses to the packet RAM,
 *   2. sends packets through the packet RAM (and the suspected DMA address) and logs what the registers do,
 *   3. logs where received OUT packets land (snapshots of the packet RAM in usb_dma_rx_snap).
 * tools/probe_usb_dma.py reads EP1 IN meanwhile, checks every packet against the expected pattern and then sends OUT packets.
 *
 * Everything it writes (SPR 0x9844, 0x9850) is restored afterwards. It wipes the packet RAM. It never touches the SD card.
 */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define USB(offset) (0xa000u + 4u * (unsigned)(offset))
enum { POWER = 0x01, INDEX = 0x0e, TXCSRL = 0x12, TXCSRH = 0x13, RXCSRL = 0x16 };
enum { TX_RDY = 0x01, TX_FLUSH = 0x08 };
enum { CSRH_MODE_TX = 0x20 };
#define VENDOR_CTRL 0x9844u
#define VENDOR_EP0 0x984cu
#define VENDOR_EP1 0x9850u
#define VENDOR_EP1_RX 0x9854u

static uint8_t buf[2048] __attribute__((aligned(32)));

static unsigned rd(unsigned o) { return apx_spr_read(USB(o)) & 255u; }
static void wr(unsigned o, unsigned v) { apx_spr_write(USB(o), v & 255u); }

static unsigned long us(uint32_t ticks) { return (unsigned long)((uint64_t)ticks * 1000000u / 32768u); }

static void fill(unsigned len) {
    for (unsigned i = 0; i < sizeof buf; ++i) buf[i] = (uint8_t)(i * 13u + len);
    apx_cache_flush_range(buf, buf + sizeof buf);
}

static void flush_fifo(void) {
    wr(TXCSRL, TX_FLUSH);
    wr(TXCSRL, TX_FLUSH);
}

static void pio_timing(void) {
    wr(INDEX, 1);
    wr(TXCSRH, rd(TXCSRH) | CSRH_MODE_TX);
    enum { ROUNDS = 100 };
    uint32_t t0 = apx_ticks();
    for (unsigned r = 0; r < ROUNDS; ++r) {
        for (unsigned i = 0; i < 512; ++i) apx_spr_write(0xa090u, buf[i]);
        flush_fifo();
    }
    unsigned long total = us(apx_ticks() - t0);
    printf("pio: 512-byte FIFO write + flush %lu us each\n", total / ROUNDS);
    wr(TXCSRH, rd(TXCSRH) & ~(unsigned)CSRH_MODE_TX);
    wr(INDEX, 0);
}

#define PRAM0 ((volatile uint32_t *)(uintptr_t)0x8040)
#define PRAM1 ((volatile uint32_t *)(uintptr_t)0x8240)
static void pram_fill(volatile uint32_t *dst, unsigned mult, unsigned len, unsigned words) {
    for (unsigned w = 0; w < words; ++w) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4; ++b) v |= (uint32_t)(uint8_t)((w * 4 + b) * mult + len) << (8 * b);
        dst[w] = v;
    }
}

/* variant 0: data in both packet RAM buffers, 0x9844 = len | 1<<14, TxPktRdy; variant 1: TxPktRdy only; variant 2: 0x9844 = len (no start bit) then TxPktRdy */
static void ram_send(unsigned len, unsigned variant) {
    pram_fill(PRAM0, 13, len, 128);
    pram_fill(PRAM1, 7, len, 128);
    wr(INDEX, 1);
    const uint32_t ctrl0 = apx_spr_read(VENDOR_CTRL);
    wr(TXCSRH, rd(TXCSRH) | CSRH_MODE_TX);
    if (variant == 0) apx_spr_write(VENDOR_CTRL, len | 1u << 14);
    if (variant == 2) apx_spr_write(VENDOR_CTRL, len);
    const uint32_t ctrl1 = apx_spr_read(VENDOR_CTRL);
    uint32_t t0 = apx_ticks();
    wr(TXCSRL, TX_RDY);
    unsigned timeout = 0;
    while (rd(TXCSRL) & TX_RDY) {
        apx_maintenance();
        if (apx_ticks() - t0 > 32768u * 2) { timeout = 1; break; }
    }
    printf("ram variant %u len %u: ctrl %08x->%08x->%08x csr %02x %s\n", variant, len, (unsigned)ctrl0, (unsigned)ctrl1,
           (unsigned)apx_spr_read(VENDOR_CTRL), rd(TXCSRL), timeout ? "TIMEOUT" : "sent");
    if (timeout) flush_fifo();
    apx_spr_write(VENDOR_CTRL, ctrl0);
    wr(TXCSRH, rd(TXCSRH) & ~(unsigned)CSRH_MODE_TX);
    wr(INDEX, 0);
}

static void pram_timing(void) {
    enum { ROUNDS = 200 };
    uint32_t t0 = apx_ticks();
    for (unsigned r = 0; r < ROUNDS; ++r) for (unsigned w = 0; w < 128; ++w) PRAM0[w] = w;
    unsigned long wr_us = us(apx_ticks() - t0);
    volatile uint32_t sink = 0;
    t0 = apx_ticks();
    for (unsigned r = 0; r < ROUNDS; ++r) for (unsigned w = 0; w < 128; ++w) sink += PRAM0[w];
    unsigned long rd_us = us(apx_ticks() - t0);
    (void)sink;
    printf("pram: 512 bytes as 128 word stores %lu.%lu us, as 128 word loads %lu.%lu us\n", wr_us / ROUNDS, wr_us * 10 / ROUNDS % 10, rd_us / ROUNDS, rd_us * 10 / ROUNDS % 10);
}

static void dma_send(unsigned len) {
    fill(len);
    wr(INDEX, 1);
    const uint32_t ctrl0 = apx_spr_read(VENDOR_CTRL), addr0 = apx_spr_read(VENDOR_EP1);
    wr(TXCSRH, rd(TXCSRH) | CSRH_MODE_TX);
    apx_spr_write(VENDOR_EP1, (uint32_t)(uintptr_t)buf);
    apx_spr_write(VENDOR_CTRL, len | 1u << 14);
    const uint32_t ctrl1 = apx_spr_read(VENDOR_CTRL);
    const unsigned csr_before = rd(TXCSRL);
    uint32_t t0 = apx_ticks();
    wr(TXCSRL, TX_RDY);
    const unsigned csr_now = rd(TXCSRL);
    unsigned timeout = 0;
    while (rd(TXCSRL) & TX_RDY) {
        apx_maintenance();
        if (apx_ticks() - t0 > 32768u * 2) { timeout = 1; break; }
    }
    unsigned long took = us(apx_ticks() - t0);
    printf("dma len %u: ctrl %08x->%08x->%08x addr %08x csr %02x/%02x/%02x %s %lu us\n", len, (unsigned)ctrl0, (unsigned)ctrl1,
           (unsigned)apx_spr_read(VENDOR_CTRL), (unsigned)apx_spr_read(VENDOR_EP1), csr_before, csr_now, rd(TXCSRL),
           timeout ? "TIMEOUT" : "sent", took);
    if (timeout) flush_fifo();
    apx_spr_write(VENDOR_EP1, addr0);
    apx_spr_write(VENDOR_CTRL, ctrl0);
    wr(TXCSRH, rd(TXCSRH) & ~(unsigned)CSRH_MODE_TX);
    wr(INDEX, 0);
}

/* Where does a received packet land? Log RXCOUNT, the vendor registers and the start of both packet RAM buffers for each OUT packet. */
uint32_t usb_dma_rx_snap[8][512] __attribute__((aligned(4), used));
uint32_t usb_dma_rx_info[8][4] __attribute__((used));
static void rx_phase(unsigned seconds) {
    wr(INDEX, 1);
    for (unsigned w = 0; w < 512; ++w) *(volatile uint32_t *)(uintptr_t)(0x8000 + 4 * w) = 0; /* diff the snapshots against zeros */
    uint32_t until = apx_ticks() + 32768u * seconds;
    unsigned n = 0;
    while (!apx_deadline_expired(until) && n < 12) {
        apx_maintenance();
        if (!(rd(RXCSRL) & 1)) continue;
        const unsigned count = rd(0x18) | rd(0x19) << 8;
        printf("rx %u: count %u csr %02x ctrl %08x a50 %08x a54 %08x | ram0 %08x %08x %08x | ram1 %08x %08x %08x\n", ++n, count, rd(RXCSRL),
               (unsigned)apx_spr_read(VENDOR_CTRL), (unsigned)apx_spr_read(VENDOR_EP1), (unsigned)apx_spr_read(VENDOR_EP1_RX),
               (unsigned)PRAM0[0], (unsigned)PRAM0[1], (unsigned)PRAM0[127], (unsigned)PRAM1[0], (unsigned)PRAM1[1], (unsigned)PRAM1[127]);
        if (n <= 8) {
            for (unsigned w = 0; w < 512; ++w) usb_dma_rx_snap[n - 1][w] = *(volatile uint32_t *)(uintptr_t)(0x8000 + 4 * w);
            usb_dma_rx_info[n - 1][0] = count; usb_dma_rx_info[n - 1][1] = (uint32_t)apx_spr_read(VENDOR_CTRL);
            usb_dma_rx_info[n - 1][2] = (uint32_t)apx_spr_read(VENDOR_EP1); usb_dma_rx_info[n - 1][3] = (uint32_t)apx_spr_read(VENDOR_EP1_RX);
        }
        wr(RXCSRL, 0);
        for (volatile unsigned d = 0; d < 20000; ++d) {}
    }
    wr(INDEX, 0);
}

int main(void) {
    printf("usb-dma-probe starting\n");
    apx_usb_init();
    uint32_t until = apx_ticks() + 32768u * 12;
    while (!apx_deadline_expired(until)) apx_debug_poll();  /* time to start the host reader */
    printf("vendor regs: 0x9844 %08x 0x984c %08x 0x9850 %08x 0x9854 %08x\n", (unsigned)apx_spr_read(VENDOR_CTRL),
           (unsigned)apx_spr_read(VENDOR_EP0), (unsigned)apx_spr_read(VENDOR_EP1), (unsigned)apx_spr_read(VENDOR_EP1_RX));
    fill(512);
    (void)dma_send;
    pio_timing();
    pram_timing();
    static const struct { unsigned len, variant; } plan[] = { {512, 0}, {512, 0}, {512, 0}, {100, 0}, {512, 1}, {512, 1}, {64, 2}, {64, 2}, {200, 1}, {512, 0} };
    for (unsigned i = 0; i < sizeof plan / sizeof plan[0]; ++i) {
        ram_send(plan[i].len, plan[i].variant);
        uint32_t wait = apx_ticks() + 32768u / 2;
        while (!apx_deadline_expired(wait)) apx_maintenance();
    }
    printf("rx phase\n");
    rx_phase(40);
    printf("usb-dma-probe done\n");
    for (;;) apx_debug_poll();
}
