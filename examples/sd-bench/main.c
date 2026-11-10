/* SD speed bench and clock probe (READ-ONLY: it only ever issues CMD13/CMD17/CMD18 and never writes the card).
 *
 * Output goes to the APXD log: `apxtool debug logs`. Run it per card; the card's identity is printed first.
 *
 * Test 1, clock calibration: a raw CMD13 (48 command bits + 48 response bits + gaps) takes a fixed number of SD clocks, so
 * the time per command against the clock divider gives the real SD clock, whatever the divider formula is. The divider
 * register is SPR 0x9888. The SDK writes divider = 144e6/(2*f) - 1 for "f"; the stock firmware writes 144e6/f - 1. */
#include <appotech/soc.h>
#include <appotech/sd.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <stdio.h>
#include <string.h>
#if APX_USB_TINYUSB
#include "tusb.h"
#endif

#ifndef APX_BENCH_RAW /* 1 = also run the raw clock probe and CMD17/CMD18 experiments (long output) */
#define APX_BENCH_RAW 0
#endif
#define CONTROL 0x9880u
#define EVENT 0x9884u
#define CLOCK 0x9888u
#define LONG_RESPONSE 0x988cu
#define COMMAND 0x9898u
#define ARGUMENT 0x989cu
#define CMD_DONE (1u << 17)
#define DATA_DONE (1u << 15)

static void ack(uint32_t mask) { apx_spr_write(EVENT, (apx_spr_read(EVENT) & ~(CMD_DONE | DATA_DONE)) | mask); }

/* mode 1 = R1 response, 5 = R1b (busy), as the SDK driver's command() */
static int raw_cmd_mode(unsigned cmd, uint32_t arg, unsigned mode, uint32_t *resp) {
    apx_spr_write(COMMAND, cmd | 0x40u);
    apx_spr_write(ARGUMENT, arg);
    ack(CMD_DONE);
    apx_spr_write(LONG_RESPONSE, 0x2000);
    apx_spr_write(EVENT, mode);
    uint32_t deadline = apx_ticks() + 3277;
    while (!(apx_spr_read(CONTROL) & CMD_DONE))
        if (apx_deadline_expired(deadline)) return -1;
    if (resp) *resp = apx_spr_read(ARGUMENT);
    ack(CMD_DONE);
    return (apx_spr_read(CONTROL) & (1u << 19)) ? -2 : 0;
}

static int raw_cmd(unsigned cmd, uint32_t arg, uint32_t *resp) { return raw_cmd_mode(cmd, arg, 1, resp); }

static unsigned long us(uint32_t ticks) { return (unsigned long)(((uint64_t)ticks * 1000000u) >> 15); }

static void print_card(const struct apx_sd_info *i) {
    const uint8_t *c = i->cid;
    printf("card: MID %02x OID %c%c name %c%c%c%c%c rev %u.%u serial %02x%02x%02x%02x date %x%02x\n", c[0], c[1], c[2], c[3], c[4], c[5], c[6],
           c[7], c[8] >> 4, c[8] & 15, c[9], c[10], c[11], c[12], c[13] & 15, c[14]);
    printf("card: %lu sectors (%lu MiB), %s, CSD ver %u, TRAN_SPEED 0x%02x, OCR 0x%08lx, write-protect %u\n", (unsigned long)i->sectors,
           (unsigned long)(i->sectors >> 11), i->high_capacity ? "SDHC/SDXC" : "SDSC", i->csd[0] >> 6, i->csd[3], (unsigned long)i->ocr,
           i->write_protected);
}

__attribute__((unused)) static void clock_probe(const struct apx_sd_info *i) {
    static const unsigned dividers[] = {359, 179, 89, 44, 22, 11, 8, 5, 3, 2};
    unsigned long first_us = 0, first_d = 0, last_us = 0, last_d = 0;
    printf("clock probe: CMD13 round trips (R1), 200 each, SDK init divider is %u\n", 359u);
    for (unsigned k = 0; k < sizeof(dividers) / sizeof(dividers[0]); ++k) {
        unsigned d = dividers[k], n = 200, fails = 0;
        apx_spr_write(CLOCK, d);
        uint32_t start = apx_ticks();
        for (unsigned j = 0; j < n; ++j) {
            if (raw_cmd(13, i->rca, 0) != 0) ++fails;
            if ((j & 31) == 31) apx_maintenance();
        }
        unsigned long total = us(apx_ticks() - start), per = total / n;
        printf("  divider %3u: %5lu us per CMD13 (%lu total, %u failed)\n", d, per, total, fails);
        if (!first_us) { first_us = per; first_d = d + 1; }
        last_us = per; last_d = d + 1;
    }
    /* t = a + b*(d+1): b in ns per divider step from the two ends, which cancels the fixed per-command software cost */
    long db = (long)first_d - (long)last_d;
    long dt_ns = ((long)first_us - (long)last_us) * 1000;
    printf("clock probe: slope %ld ns per divider step (d+1)\n", dt_ns / db);
    printf("  one step is 1/144e6 = 6.94 ns if SD clock = 144e6/(d+1); with 112 clocks per command expect 778 ns per step (x2 = 1556 if clock = 144e6/(2(d+1)))\n");
}

/* Is the divider register 8 or 9 bits wide, and does a new divider apply at once? */
static unsigned long timed(unsigned d, unsigned n, const struct apx_sd_info *i) {
    apx_spr_write(CLOCK, d);
    uint32_t start = apx_ticks();
    for (unsigned j = 0; j < n; ++j) {
        raw_cmd(13, i->rca, 0);
        if ((j & 31) == 31) apx_maintenance();
    }
    return us(apx_ticks() - start) / n;
}

__attribute__((unused)) static void width_probe(const struct apx_sd_info *i) {
    printf("width probe (CMD13 us, 100 each; steps = d+1):\n");
    static const unsigned pairs[][2] = {{359, 103}, {300, 44}, {267, 11}, {256, 0}, {255, 255}, {200, 200}, {600, 88}};
    for (unsigned k = 0; k < sizeof(pairs) / sizeof(pairs[0]); ++k) {
        unsigned a = pairs[k][0], b = pairs[k][1];
        unsigned long ta = timed(a, 100, i), tb = timed(b, 100, i);
        printf("  d=%3u: %4lu us   d=%3u (= %u mod 256): %4lu us   -> %s\n", a, ta, b, a, tb,
               a == b ? "(same value, repeatability)" : (ta + ta / 8 >= tb && ta <= tb + tb / 8 + 2) ? "same time: 8-bit register" : "different: wider than 8 bits");
    }
    /* a fast clock, then 359: how long do the first commands take? */
    apx_spr_write(CLOCK, 2);
    raw_cmd(13, i->rca, 0);
    apx_spr_write(CLOCK, 359);
    printf("first 12 commands after switching 2 -> 359 (us each):");
    for (unsigned j = 0; j < 12; ++j) {
        uint32_t t0 = apx_ticks();
        raw_cmd(13, i->rca, 0);
        printf(" %lu", us(apx_ticks() - t0));
    }
    printf("\n");
}


/* ---- read-path experiments: raw CMD17 / CMD18 sequences as the stock firmware issues them ---- */
#define DMA_ADDRESS 0x9890u
#define DMA_LENGTH 0x9894u
#define CARD_READY (1u << 20)
#define REF ((uint8_t *)0x02000000u) /* free DDR below the runtime window: 256 KiB each */
#define TST ((uint8_t *)0x02040000u)
#define GUARD ((uint8_t *)0x02080000u)
#define NSEC 512u
static unsigned test_lba = 100000; /* chosen by pick_region(): a window that holds real data */

struct stats { unsigned hist[4], err16, bit11, fails, mismatches; unsigned long us; };

static void rx_arm(void *p, unsigned len) { /* stock 0x2074f24 */
    apx_spr_write(DMA_ADDRESS, (uintptr_t)p);
    ack(DATA_DONE);
    apx_spr_write(DMA_LENGTH, len);
    apx_spr_write(EVENT, apx_spr_read(EVENT) | 0x10u);
    apx_spr_write(CONTROL, apx_spr_read(CONTROL) & ~2u);
}

static int rx_wait(struct stats *st) { /* stock 0x2075040 then the 0x20750d0 status check, recorded instead of enforced */
    uint32_t deadline = apx_ticks() + 16384, c;
    for (;;) {
        c = apx_spr_read(CONTROL);
        if (c & DATA_DONE) break;
        if (apx_deadline_expired(deadline)) { ++st->fails; return -1; }
    }
    ack(DATA_DONE);
    st->hist[(c >> 21) & 3]++;
    if (c & (1u << 16)) ++st->err16;
    if (c & (1u << 11)) ++st->bit11;
    return 0;
}

static void stop_stream(void) { /* CMD12, clock DAT0 until the card releases it, then CMD13 until it is back in the transfer state */
    raw_cmd_mode(12, 0, 5, 0);
    apx_spr_write(CONTROL, apx_spr_read(CONTROL) | 2u);
    uint32_t deadline = apx_ticks() + 3277;
    while (!(apx_spr_read(CONTROL) & CARD_READY) && !apx_deadline_expired(deadline)) {}
    apx_spr_write(CONTROL, apx_spr_read(CONTROL) & ~2u);
    apx_spr_write(DMA_LENGTH, 0);
    for (unsigned j = 0; j < 64; ++j) {
        uint32_t r = 0;
        if (raw_cmd(13, apx_sd_get_info()->rca, &r) == 0 && (r & 0x1e00u) == 0x800u) break;
    }
}

static unsigned lba_arg(unsigned lba) { return apx_sd_get_info()->high_capacity ? lba : lba * 512u; }

/* single-block reads, one CMD17 each */
static void read_singles(uint8_t *dst, unsigned lba, unsigned n, struct stats *st) {
    apx_dma_invalidate(dst, n * 512);
    for (unsigned i = 0; i < n; ++i) {
        rx_arm(dst + i * 512, 512);
        if (raw_cmd(17, lba_arg(lba + i), 0) != 0) { ++st->fails; apx_spr_write(DMA_LENGTH, 0); continue; }
        rx_wait(st);
        apx_spr_write(DMA_LENGTH, 0);
        if ((i & 31) == 31) apx_maintenance();
    }
    apx_dma_invalidate(dst, n * 512);
}

/* one open-ended CMD18, the DMA re-armed for `per` sectors at a time (stock: per = 1) */
static void read_stream(uint8_t *dst, unsigned n, unsigned per, struct stats *st) {
    apx_dma_invalidate(dst, n * 512);
    rx_arm(dst, 512 * per);
    if (raw_cmd(18, lba_arg(test_lba), 0) != 0) { ++st->fails; apx_spr_write(DMA_LENGTH, 0); return; }
    for (unsigned i = 0; i < n; i += per) {
        if (i) rx_arm(dst + i * 512, 512 * per);
        if (rx_wait(st)) break;
        if ((i & 63) == 63) apx_maintenance();
    }
    stop_stream();
    apx_dma_invalidate(dst, n * 512);
}

static void report(const char *label, unsigned d, unsigned bits, uint8_t *dst, struct stats *st, unsigned n) {
    for (unsigned i = 0; i < n * 512; ++i) if (dst[i] != REF[i]) { ++st->mismatches; i |= 511; }
    unsigned long kbps = st->us ? (unsigned long)((uint64_t)n * 512 * 1000000u / st->us / 1000u) : 0;
    printf("  %-22s d=%u (%2lu MHz) bits=%s: %4lu KB/s | st22:21 [%u %u %u %u] err16=%u bit11=%u fails=%u badsectors=%u\n", label, d,
           144ul / (d + 1), (bits & 0x80000008u) ? "HS" : "--", kbps, st->hist[0], st->hist[1], st->hist[2], st->hist[3], st->err16, st->bit11,
           st->fails, st->mismatches);
}

static void set_clock_bits(unsigned d, int hs) {
    uint32_t c = apx_spr_read(CONTROL) & ~(0x80000000u | 8u);
    if (hs) c |= 0x80000000u, apx_spr_write(CONTROL, c), c |= 8u;
    apx_spr_write(CONTROL, c);
    apx_spr_write(CLOCK, d);
}

__attribute__((unused)) static void read_experiments(void) {
    /* reference copy through the SDK driver (single blocks); the first sector is also compared offline with the debug-link read */
    if (apx_sd_read(test_lba, REF, NSEC) != 0) { printf("reference read failed\n"); return; }
    apx_sd_sync(); /* the read leaves a CMD18 stream (and a read-ahead DMA) open: raw commands fail until the driver has stopped it */
    uint32_t sum = 0;
    for (unsigned i = 0; i < NSEC * 512 / 4; ++i) sum += ((uint32_t *)REF)[i];
    printf("reference: %u sectors at LBA %u, word sum %08lx, first bytes %02x %02x %02x %02x\n", NSEC, test_lba, (unsigned long)sum, REF[0], REF[1], REF[2], REF[3]);
    printf("read experiments (%u sectors = %u KiB each):\n", NSEC, NSEC / 2);
    static const unsigned dividers[] = {5, 3, 2};
    for (unsigned k = 0; k < 3; ++k) {
        for (unsigned hs = 0; hs < 2; ++hs) {
            unsigned d = dividers[k];
            struct stats a, b;
            memset(&a, 0, sizeof(a));
            memset(&b, 0, sizeof(b));
            set_clock_bits(d, hs);
            memset(TST, 0xee, NSEC * 512);
            apx_dma_clean(TST, NSEC * 512);
            uint32_t t0 = apx_ticks();
            read_singles(TST, test_lba, 128, &a);
            a.us = us(apx_ticks() - t0);
            report("CMD17 x128", d, hs ? 0x80000008u : 0, TST, &a, 128);
            memset(TST, 0xee, NSEC * 512);
            apx_dma_clean(TST, NSEC * 512);
            t0 = apx_ticks();
            read_stream(TST, NSEC, 1, &b);
            b.us = us(apx_ticks() - t0);
            report("CMD18 stream x512", d, hs ? 0x80000008u : 0, TST, &b, NSEC);
        }
    }
    /* how many sectors can one DMA arm cover? (d=5, default-speed bits; the guard area after the buffer must stay untouched) */
    set_clock_bits(5, 0);
    for (unsigned per = 2; per <= 8; per *= 2) {
        struct stats c;
        memset(&c, 0, sizeof(c));
        memset(GUARD, 0x77, 4096);
        apx_dma_clean(GUARD, 4096);
        memset(TST, 0xee, NSEC * 512);
        apx_dma_clean(TST, NSEC * 512);
        uint32_t t0 = apx_ticks();
        read_stream(TST, NSEC, per, &c);
        c.us = us(apx_ticks() - t0);
        char label[32];
        snprintf(label, sizeof(label), "CMD18, %u sectors/arm", per);
        report(label, 5, 0, TST, &c, NSEC);
    }
    apx_dma_invalidate(GUARD, 4096);
    unsigned guard_bad = 0;
    for (unsigned i = 0; i < 4096; ++i) guard_bad += GUARD[i] != 0x77;
    printf("  guard area after the buffer: %u bytes changed\n", guard_bad);
    set_clock_bits(5, 0);
}


/* A sector is "empty" if every byte equals the first (erased flash reads as all 0x00 or all 0xff): comparing empty data proves little. */
static unsigned data_sectors(const uint8_t *buf, unsigned n) {
    unsigned count = 0;
    for (unsigned k = 0; k < n; ++k) {
        const uint8_t *q = buf + k * 512;
        unsigned uniform = 1;
        for (unsigned b = 1; b < 512; ++b) if (q[b] != q[0]) { uniform = 0; break; }
        count += !uniform;
    }
    return count;
}

static void pick_region(void) {
    static const unsigned candidates[] = {100000, 8192, 32768, 65536, 262144, 524288, 1048576, 2097152, 4194304, 8388608, 16777216};
    const uint64_t sectors = apx_sd_get_info()->sectors;
    unsigned best = 0, best_n = 0;
    for (unsigned k = 0; k < sizeof(candidates) / sizeof(candidates[0]); ++k) {
        if ((uint64_t)candidates[k] + NSEC > sectors || apx_sd_read(candidates[k], REF, NSEC) != 0) continue;
        unsigned n = data_sectors(REF, NSEC);
        if (n > best_n) { best = candidates[k]; best_n = n; }
        if (n == NSEC) break;
    }
    test_lba = best_n ? best : 100000;
    printf("test region: LBA %u, %u of %u sectors hold data%s\n", test_lba, best_n, NSEC, best_n < NSEC / 4 ? "  (!) mostly empty: weak integrity check" : "");
}

/* The very end of the card: the largest sector numbers, through the driver and through a raw CMD17 path. */
static void last_sector_test(void) {
    const uint64_t sectors = apx_sd_get_info()->sectors;
    unsigned last = (unsigned)(sectors - 8);
    unsigned saved = 144000000u / apx_sd_get_info()->clock_hz - 1;
    int r = apx_sd_read(last, TST, 8);
    apx_sd_sync(); /* stop the driver's stream before issuing raw commands */
    struct stats st;
    memset(&st, 0, sizeof(st));
    apx_spr_write(CLOCK, 11);
    read_singles(REF, last, 8, &st);
    apx_spr_write(CLOCK, saved);
    unsigned same = r == 0 && st.fails == 0 && memcmp(TST, REF, 8 * 512) == 0;
    printf("last 8 sectors (LBA %u..%u): driver read %d, raw reference fails %u, %s; non-empty sectors %u\n", last, last + 7, r, st.fails,
           same ? "identical" : "DIFFERENT", data_sectors(TST, 8));
    printf("  after it: driver initialized %u, errors %lu, retries %lu, last command %lu\n", apx_sd_get_info()->initialized,
           (unsigned long)apx_sd_get_info()->errors, (unsigned long)apx_sd_get_info()->retries, (unsigned long)apx_sd_get_info()->last_command);
}

/* ---- what the SDK driver itself delivers, checked against an independent reference ---- */
static unsigned long kbps(unsigned sectors, unsigned long micro) { return micro ? (unsigned long)((uint64_t)sectors * 512u * 1000000u / micro / 1000u) : 0; }

static void driver_bench(void) {
    const struct apx_sd_info *i = apx_sd_get_info();
    printf("driver: %lu Hz, high speed %u, bus %u bit, wide probe %u detail %06lx, streams %lu\n", (unsigned long)i->clock_hz, i->high_speed, i->bus_width,
           i->bus_probe, (unsigned long)i->bus_probe_detail, (unsigned long)i->streams);
    /* the reference: raw CMD17 singles at 12 MHz (divider 11), bypassing the driver's read path entirely */
    apx_sd_sync(); /* the driver may have a CMD18 stream open (pick_region read through it): stop it before raw commands */
    unsigned saved = 144000000u / i->clock_hz - 1; /* SPR 0x9888 is write-only: derive it from the driver's clock */
    struct stats ref;
    memset(&ref, 0, sizeof(ref));
    apx_spr_write(CLOCK, 11);
    read_singles(REF, test_lba, NSEC, &ref);
    apx_spr_write(CLOCK, saved);
    uint32_t sum = 0;
    for (unsigned w = 0; w < NSEC * 512 / 4; ++w) sum += ((uint32_t *)REF)[w];
    printf("reference (raw CMD17 at 12 MHz, %u fails): word sum %08lx\n", ref.fails, (unsigned long)sum);
    static const char *names[] = {"1 call x512 sectors", "512 calls x1 sequential", "64 calls x8 sectors", "512 calls x1 random order",
                                  "1 call x512, unaligned buffer"};
    /* each pattern three times: a one-off slowdown (a card still busy after power-up, say) shows as an outlier, not as the card's speed */
    for (unsigned t = 0; t < 5; ++t) {
        uint8_t *dst = TST + (t == 4 ? 1 : 0);
        unsigned long rate[3] = {0, 0, 0}, worst = 0;
        unsigned errors = 0, bad = 0;
        for (unsigned rep = 0; rep < 3; ++rep) {
            memset(TST, 0xee, NSEC * 512 + 16);
            apx_dma_clean(TST, NSEC * 512 + 16);
            uint32_t t0 = apx_ticks();
            if (t == 0 || t == 4) errors += apx_sd_read(test_lba, dst, NSEC) != 0;
            if (t == 1) for (unsigned k = 0; k < NSEC; ++k) errors += apx_sd_read(test_lba + k, dst + k * 512, 1) != 0;
            if (t == 2) for (unsigned k = 0; k < NSEC; k += 8) errors += apx_sd_read(test_lba + k, dst + k * 512, 8) != 0;
            if (t == 3) /* a fixed shuffle: stride 211 is coprime with 512 */
                for (unsigned k = 0; k < NSEC; ++k) { unsigned n = (k * 211u) % NSEC; errors += apx_sd_read(test_lba + n, dst + n * 512, 1) != 0; }
            unsigned long took = us(apx_ticks() - t0);
            apx_dma_invalidate(TST, NSEC * 512 + 16);
            for (unsigned b = 0; b < NSEC * 512; ++b) if (dst[b] != REF[b]) { ++bad; b |= 511; }
            rate[rep] = kbps(NSEC, took);
            if (took > worst) worst = took;
        }
        printf("  %-30s %5lu %5lu %5lu KB/s  errors %u, sectors differing from the reference %u%s\n", names[t], rate[0], rate[1], rate[2], errors, bad,
               t == 3 ? " (random order)" : "");
    }
    printf("driver: after the runs: reads %lu, streams %lu, errors %lu, retries %lu\n", (unsigned long)i->reads, (unsigned long)i->streams,
           (unsigned long)i->errors, (unsigned long)i->retries);
}

/* The same driver read (64 calls x 8 sectors) at several SD clocks: a transfer that is limited by the bus scales with the clock (and with the bus width),
 * one that is limited by the card or the SoC does not. */
static void clock_sweep(void) {
    const struct apx_sd_info *i = apx_sd_get_info();
    static const unsigned hz[] = {6000000, 12000000, 24000000, 36000000, 48000000};
    const unsigned original = i->clock_hz;
    printf("clock sweep, %u-bit bus (64 calls x8 sectors, best of 3):\n", i->bus_width);
    for (unsigned c = 0; c < sizeof(hz) / sizeof(hz[0]); ++c) {
        if (apx_sd_set_clock(hz[c]) != 0) { printf("  %2u MHz: not accepted\n", hz[c] / 1000000u); continue; }
        unsigned long best = 0;
        unsigned errors = 0;
        for (unsigned rep = 0; rep < 3; ++rep) {
            uint32_t t0 = apx_ticks();
            for (unsigned k = 0; k < NSEC; k += 8) errors += apx_sd_read(test_lba + k, TST + k * 512, 8) != 0;
            unsigned long rate = kbps(NSEC, us(apx_ticks() - t0));
            if (rate > best) best = rate;
        }
        printf("  %2lu MHz: %5lu KB/s, errors %u\n", (unsigned long)apx_sd_get_info()->clock_hz / 1000000ul, best, errors);
    }
    apx_sd_set_clock(original);
}

/* The SD driver calls apx_usb_poll() once per sector (service()), so its cost is part of the read rate. */
static void poll_cost(void) {
    enum { N = 2000 };
    uint32_t t0 = apx_ticks();
    for (unsigned k = 0; k < N; ++k) apx_usb_poll();
    unsigned long total = us(apx_ticks() - t0);
    printf("apx_usb_poll: %lu ns per call (%d calls)\n", total * 1000 / N, N);
}

/* What does one USB register access cost? (the poll is mostly these) */
static void spr_cost(void) {
    enum { N = 4000 };
    volatile uint32_t sink = 0;
    uint32_t t0 = apx_ticks();
    for (unsigned k = 0; k < N; ++k) sink += apx_spr_read(0xa000u + 4u * 0x0e);   /* USB INDEX (a harmless read) */
    unsigned long read_ns = us(apx_ticks() - t0) * 1000 / N;
    t0 = apx_ticks();
    for (unsigned k = 0; k < N; ++k) apx_spr_write(0xa000u + 4u * 0x0e, 0);       /* select endpoint 0 */
    unsigned long write_ns = us(apx_ticks() - t0) * 1000 / N;
    t0 = apx_ticks();
    for (unsigned k = 0; k < N; ++k) sink += apx_spr_read(0x9804);                  /* the timer, for comparison: a non-USB SPR */
    unsigned long timer_ns = us(apx_ticks() - t0) * 1000 / N;
    {   /* which USB registers are slow? (INTR* reads clear flags: harmless here, the poll tolerates a lost one) */
        static const struct { const char *name; unsigned off; } regs[] = {
            {"POWER", 0x01}, {"INTRTX", 0x02}, {"INTRRX", 0x04}, {"INTRUSB", 0x0a}, {"CSR0L/TXCSRL", 0x12}, {"RXCSRL", 0x16}, {"RXCOUNT", 0x18},
        };
        apx_spr_write(0xa000u + 4u * 0x0e, 0);
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
            uint32_t r0 = apx_ticks();
            for (unsigned k = 0; k < N; ++k) sink += apx_spr_read(0xa000u + 4u * regs[i].off);
            printf("  USB reg %-13s read %lu ns\n", regs[i].name, us(apx_ticks() - r0) * 1000 / N);
        }
    }
    printf("SPR access: USB read %lu ns, USB write %lu ns, timer read %lu ns (%u calls each)\n", read_ns, write_ns, timer_ns, N);
    (void)sink;
}

#if APX_USB_TINYUSB
/* Where does one apx_usb_poll() go? Each piece timed on its own. */
static void poll_parts(void) {
    enum { N = 2000 };
    uint32_t t0 = apx_ticks();
    for (unsigned k = 0; k < N; ++k) dcd_int_handler(0);
    unsigned long dcd = us(apx_ticks() - t0) * 1000 / N;
    t0 = apx_ticks();
    for (unsigned k = 0; k < N; ++k) tud_task_ext(0, false);
    unsigned long task = us(apx_ticks() - t0) * 1000 / N;
    t0 = apx_ticks();
    for (unsigned k = 0; k < N; ++k) apx_maintenance();
    unsigned long maint = us(apx_ticks() - t0) * 1000 / N;
    t0 = apx_ticks();
    for (unsigned k = 0; k < N; ++k) (void)apx_ticks();
    unsigned long ticks = us(apx_ticks() - t0) * 1000 / N;
    printf("poll parts: dcd_int_handler %lu ns, tud_task_ext %lu ns, apx_maintenance %lu ns, apx_ticks %lu ns\n", dcd, task, maint, ticks);
}
#endif

int main(void) {
    printf("sd-bench starting\n");
    apx_usb_init();
    spr_cost();
    poll_cost();
#if APX_USB_TINYUSB
    poll_parts();
#endif
    uint32_t init_t0 = apx_ticks();
    int r = apx_sd_init();
    printf("sd init: %d, took %lu ms\n", r, us(apx_ticks() - init_t0) / 1000ul);
    for (unsigned again = 1; r && again <= 4; ++again) { /* diagnosis: does a later init succeed (timing) or does the card need a power cycle? */
        uint32_t until = apx_ticks() + 32768u * again;
        while (!apx_deadline_expired(until)) apx_debug_poll();
        r = apx_sd_init();
        printf("sd init again #%u after %u s: %d (bus probe %u)\n", again, again, r, apx_sd_get_info()->bus_probe);
    }
    if (r) {
        const struct apx_sd_info *f = apx_sd_get_info();
        printf("sd init failed: last command %lu status %08lx ocr %08lx errors %lu wide probe %u detail %06lx bus %u\n", (unsigned long)f->last_command, (unsigned long)f->last_status,
               (unsigned long)f->ocr, (unsigned long)f->errors, f->bus_probe, (unsigned long)f->bus_probe_detail, f->bus_width);
    }
    if (r == 0) {
        print_card(apx_sd_get_info());
        pick_region();
        driver_bench();
        clock_sweep();
        last_sector_test();
#if APX_BENCH_RAW
        apx_sd_sync(); /* the driver leaves a CMD18 stream open between calls; raw commands need it stopped */
        clock_probe(apx_sd_get_info());
        width_probe(apx_sd_get_info());
        read_experiments();
#endif
        apx_sd_shutdown(); /* leave the controller idle; nothing was written */
    }
    printf("sd-bench done\n");
    for (;;) apx_debug_poll();
}
