/* Card-by-card SD write test and hot-removal test. WRITES TO THE INSERTED CARDS (the owner approved it for his test cards, 2026-11-08).
 *
 * Footprint on every card, so that nothing the owner keeps on it can be damaged by the test itself:
 *   - raw: only sectors of the gap between the MBR and the first partition that are ALL ZERO, at most 62 of them from LBA 1; never on a GPT disk, never without a valid
 *     MBR; saved first, restored and compared at the end;
 *   - files: one scratch file 0:/APXWTEST.BIN on a FAT or exFAT volume with enough free space (never overwriting a file of that name), written, read back, deleted.
 * Nothing is formatted or otherwise changed. (A card pulled in the middle of the file test can still leave the scratch file's FAT entries half written.)
 *
 * APX_SCT_MODE 0 (default): CARDS. Waits for a card, runs the suite (raw tests, then the file test with rates), reports one result block, waits for the card to be
 *   removed, waits for the next. Results are also kept in `sct_cards[]`.
 * APX_SCT_MODE 1: HOT-PULL. Keeps one card busy in a rotation of idle (a sector read every 100 ms), read stream and write stream phases for APX_SCT_SECONDS; every
 *   time the card is pulled the driver's error is logged with the phase, how long the call took, and then the time until the card is back after reinsertion.
 *   The raw writes go only to the gap; at the end every gap sector must hold one of the last three generations written (no torn or garbage sector), and the gap
 *   is restored. */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/sd.h>
#include <appotech/storage.h>
#include <stdio.h>
#include <string.h>

#ifndef APX_SCT_MODE
#define APX_SCT_MODE 0
#endif
#ifndef APX_SCT_REPEAT
#define APX_SCT_REPEAT 1 /* mode 0: run the suite this many times on the same card (to catch an intermittent failure) */
#endif
#ifndef APX_SCT_SECONDS
#define APX_SCT_SECONDS 300
#endif
#ifndef APX_SCT_FILE_MIB
#define APX_SCT_FILE_MIB 8
#endif
#define TICKS 32768u
#define BUF_BYTES 65536u
static uint8_t buf[BUF_BYTES + 64] __attribute__((section(".large_buffer"), aligned(32)));
static uint8_t back[BUF_BYTES + 64] __attribute__((section(".large_buffer"), aligned(32)));
static uint8_t snapshot[62 * 512] __attribute__((section(".large_buffer"), aligned(32)));

struct card_result {
    char name[8];
    uint32_t serial, sectors, flags, fails, steps, errors, retries, wstreams, write_blocks, bad_tokens;
    uint32_t kbps[4]; /* file write 512 B / 4 KiB / 64 KiB chunks, file read */
    uint32_t gap;     /* raw test sectors used, 0 = raw tests skipped */
} sct_cards[16] __attribute__((used));
uint32_t sct_count __attribute__((used));

static unsigned long ms(uint32_t ticks) { return (unsigned long)((uint64_t)ticks * 1000u / TICKS); }
static uint8_t pattern(uint32_t seed, uint64_t offset) { return (uint8_t)((offset >> 9) * 131u + (offset & 511u) * 7u + seed * 29u + 1u); }
static void fill(uint8_t *p, uint32_t seed, uint64_t offset, size_t n) { for (size_t i = 0; i < n; ++i) p[i] = pattern(seed, offset + i); }
static int same(const uint8_t *p, uint32_t seed, uint64_t offset, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i] != pattern(seed, offset + i)) return 0; return 1; }
static int token_ok(void) { return (apx_sd_get_info()->last_write_status & 0x600000u) == 0x400000u; }
static void idle_wait(uint32_t ticks) { uint32_t until = apx_ticks() + ticks; while (!apx_deadline_expired(until)) apx_debug_poll(); }

static void card_id(char name[8], uint32_t *serial) {
    const struct apx_sd_info *info = apx_sd_get_info();
    memset(name, 0, 8);
    for (unsigned i = 0; i < 5; ++i) name[i] = info->cid[3 + i] >= 32 && info->cid[3 + i] < 127 ? (char)info->cid[3 + i] : '?';
    *serial = (uint32_t)info->cid[9] << 24 | (uint32_t)info->cid[10] << 16 | (uint32_t)info->cid[11] << 8 | info->cid[12];
}

/* The sectors of the gap after the MBR this test may use: a run of all-zero sectors from LBA 1, below the first partition. Returns the count (0 = none). */
static unsigned find_gap(void) {
    if (apx_sd_read(0, buf, 1)) return 0;
    if (buf[510] != 0x55 || buf[511] != 0xaa) return 0;
    uint32_t first = 0xffffffffu;
    for (unsigned e = 0; e < 4; ++e) {
        const uint8_t *p = buf + 446 + 16 * e;
        if (p[4] == 0xee) return 0; /* GPT protective MBR: LBA 1 is the GPT header */
        const uint32_t start = p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24;
        if (p[4] && start && start < first) first = start;
    }
    if (first < 2) return 0;
    unsigned limit = first - 1 < 62 ? first - 1 : 62, run = 0;
    if (apx_sd_read(1, snapshot, limit)) return 0;
    while (run < limit) {
        int zero = 1;
        for (unsigned i = 0; i < 512; ++i) if (snapshot[run * 512 + i]) { zero = 0; break; }
        if (!zero) break;
        ++run;
    }
    return run;
}

/*--------------------------------------------------------------------+
 * Mode 0: the per-card suite
 *--------------------------------------------------------------------*/
static struct card_result *cur;
static void step(int ok, const char *what) {
    ++cur->steps;
    if (!ok) { ++cur->fails; printf("  FAIL %s\n", what); } else printf("  ok   %s\n", what);
}

static int write_and_verify(uint32_t lba, const uint8_t *source, unsigned count, uint32_t seed, int sync) {
    if (apx_sd_write(lba, source, count)) return 0;
    if (!token_ok()) { ++cur->bad_tokens; return 0; }
    if (sync && apx_sd_sync()) return 0;
    memset(back, 0, count * 512u);
    int rr = apx_sd_read(lba, back, count);
    if (rr) { printf("    verify: read error %d at LBA %lu x%u\n", rr, (unsigned long)lba, count); return 0; }
    if (!same(back, seed, (uint64_t)lba * 512u, count * 512u)) {
        unsigned bad = 0, zero = 0, first = 0xffffu;
        for (unsigned i = 0; i < count * 512u; ++i) if (back[i] != pattern(seed, (uint64_t)lba * 512u + i)) { if (first == 0xffffu) first = i; ++bad; }
        for (unsigned i = 0; i < count * 512u; ++i) zero += back[i] == 0;
        printf("    verify: LBA %lu x%u seed %lu: %u wrong bytes, first at %u (got %02x want %02x), %u zero bytes, write status %08lx\n", (unsigned long)lba, count, (unsigned long)seed, bad, first,
               back[first], pattern(seed, (uint64_t)lba * 512u + first), zero, (unsigned long)apx_sd_get_info()->last_write_status);
        return 0;
    }
    return 1;
}

static void raw_tests(unsigned gap) {
    static const unsigned sizes[] = { 1, 2, 5, 16, 62 };
    int ok = 1;
    for (unsigned k = 0; k < 5; ++k) {
        const unsigned n = sizes[k] < gap ? sizes[k] : gap;
        fill(buf, 10 + k, 512u, n * 512u);
        ok &= write_and_verify(1, buf, n, 10 + k, k & 1);
    }
    step(ok, "raw: 1,2,5,16,62-sector writes");
    fill(buf + 1, 21, 512u, 5u * 512u);
    step(write_and_verify(1, buf + 1, gap < 5 ? gap : 5, 21, 1), "raw: unaligned source");
    ok = 1;
    for (unsigned s = 1; s <= gap && ok; ++s) { fill(buf, 30, (uint64_t)s * 512u, 512u); ok = apx_sd_write(s, buf, 1) == 0 && token_ok(); }
    ok = ok && apx_sd_sync() == 0;
    memset(back, 0, gap * 512u);
    step(ok && apx_sd_read(1, back, gap) == 0 && same(back, 30, 512u, gap * 512u), "raw: sequential single sectors (stream)");
    ok = 1;
    for (unsigned r = 0; r < 6 && ok; ++r) { fill(buf, 40 + r, (uint64_t)(1 + r) * 512u, 1024u); ok = apx_sd_write(1 + r, buf, 2) == 0 && apx_sd_read(0, back + 4096, 1) == 0; }
    step(ok && apx_sd_sync() == 0 && back[4096 + 510] == 0x55, "raw: writes interleaved with reads");
}

static int file_tests(void) {
    FIL file;
    DWORD free_clusters;
    FATFS *fs;
    (void)apx_storage_unmount();
    FRESULT m = apx_storage_mount();
    if (m != FR_OK) { printf("  skip file test: mount %d (not FAT/exFAT?)\n", (int)m); return 0; }
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK || (uint64_t)free_clusters * fs->csize * 512u < (uint64_t)(APX_SCT_FILE_MIB + 4) << 20) { printf("  skip file test: not enough free space\n"); return 0; }
    FILINFO info;
    if (f_stat("0:/APXWTEST.BIN", &info) == FR_OK) { printf("  skip file test: APXWTEST.BIN exists\n"); return 0; }
    if (f_open(&file, "0:/APXWTEST.BIN", FA_CREATE_NEW | FA_WRITE | FA_READ) != FR_OK) { step(0, "create scratch file"); return 0; }
    static const struct { unsigned chunk, mib; } phases[3] = { { 512, 1 }, { 4096, 1 }, { 65536, APX_SCT_FILE_MIB - 2 } };
    uint64_t offset = 0;
    int all = 1;
    for (unsigned p = 0; p < 3; ++p) {
        const uint32_t total = phases[p].mib << 20, t0 = apx_ticks();
        int ok = 1;
        for (uint32_t done = 0; done < total && ok; done += phases[p].chunk) {
            fill(buf, 50, offset + done, phases[p].chunk);
            UINT w = 0;
            ok = f_write(&file, buf, phases[p].chunk, &w) == FR_OK && w == phases[p].chunk;
        }
        ok = ok && f_sync(&file) == FR_OK;
        const uint32_t dt = apx_ticks() - t0;
        offset += total;
        cur->kbps[p] = dt ? (uint32_t)((uint64_t)total / 1024u * TICKS / dt) : 0;
        all &= ok;
    }
    printf("  file write KB/s: 512B %lu  4K %lu  64K %lu\n", (unsigned long)cur->kbps[0], (unsigned long)cur->kbps[1], (unsigned long)cur->kbps[2]);
    step(all && f_size(&file) == offset && f_close(&file) == FR_OK, "file written, synced, closed");
    int ok = f_open(&file, "0:/APXWTEST.BIN", FA_READ) == FR_OK;
    const uint32_t t0 = apx_ticks();
    for (uint64_t at = 0; ok && at < offset; at += BUF_BYTES) { UINT got = 0; ok = f_read(&file, back, BUF_BYTES, &got) == FR_OK && got == BUF_BYTES && same(back, 50, at, BUF_BYTES); }
    const uint32_t dt = apx_ticks() - t0;
    cur->kbps[3] = dt ? (uint32_t)(offset / 1024u * TICKS / dt) : 0;
    f_close(&file);
    printf("  file read-back KB/s: %lu\n", (unsigned long)cur->kbps[3]);
    step(ok, "file read back and verified");
    step(f_unlink("0:/APXWTEST.BIN") == FR_OK && f_stat("0:/APXWTEST.BIN", &info) == FR_NO_FILE && apx_storage_sync() == FR_OK, "scratch file deleted, filesystem synced");
    (void)apx_storage_unmount();
    return 1;
}

static void run_suite(void) {
    if (sct_count >= 16) sct_count = 15;
    cur = &sct_cards[sct_count++];
    memset(cur, 0, sizeof *cur);
    const struct apx_sd_info *info = apx_sd_get_info();
    card_id(cur->name, &cur->serial);
    cur->sectors = (uint32_t)info->sectors;
    cur->flags = (info->high_capacity ? 1u : 0u) | (info->high_speed ? 2u : 0u) | (info->write_protected ? 4u : 0u);
    printf("CARD %u: %s %08lx %lu MiB %s hs%u %lu Hz wp%u\n", (unsigned)sct_count, cur->name, (unsigned long)cur->serial, (unsigned long)(info->sectors / 2048), info->high_capacity ? "SDHC" : "SDSC",
           info->high_speed, (unsigned long)info->clock_hz, info->write_protected);
    if (info->write_protected) { printf("  write-protected: no writes\n"); return; }
    const unsigned gap = find_gap();
    cur->gap = gap;
    if (gap >= 8) {
        printf("  raw gap: %u zero sectors from LBA 1\n", gap);
        raw_tests(gap);
        int ok = apx_sd_write(1, snapshot, gap) == 0 && apx_sd_sync() == 0;
        memset(back, 0x5a, gap * 512u);
        step(ok && apx_sd_read(1, back, gap) == 0 && memcmp(back, snapshot, gap * 512u) == 0, "raw: gap restored and verified");
    } else printf("  raw tests skipped (usable gap %u sectors)\n", gap);
    file_tests();
    info = apx_sd_get_info();
    cur->errors = info->errors; cur->retries = info->retries; cur->wstreams = info->wstreams; cur->write_blocks = info->writes;
    printf("  RESULT %s: %lu steps, %lu failed, writes %lu (streams %lu), bad tokens %lu, errors %lu, retries %lu\n", cur->name, (unsigned long)cur->steps, (unsigned long)cur->fails,
           (unsigned long)info->writes, (unsigned long)info->wstreams, (unsigned long)cur->bad_tokens, (unsigned long)info->errors, (unsigned long)info->retries);
}

__attribute__((unused)) static void cards_mode(void) {
    uint32_t last_serial = 0;
    char last_name[8] = "";
    unsigned announced_none = 0;
    for (;;) {
        int r = apx_sd_init();
        if (r) {
            static unsigned tries;
            const struct apx_sd_info *fi = apx_sd_get_info();
            if (!announced_none || ++tries % 5 == 0) printf("no card (%d) cmd %lu status %08lx ocr %08lx errors %lu\n", r, (unsigned long)fi->last_command, (unsigned long)fi->last_status, (unsigned long)fi->ocr, (unsigned long)fi->errors);
            announced_none = 1; last_serial = 0;
            idle_wait(TICKS);
            continue;
        }
        announced_none = 0;
        char name[8];
        uint32_t serial;
        card_id(name, &serial);
        if (serial != last_serial || memcmp(name, last_name, 8)) {
            last_serial = serial;
            memcpy(last_name, name, 8);
            for (unsigned rep = 0; rep < APX_SCT_REPEAT; ++rep) { if (rep) { printf("-- repeat %u\n", rep + 1); if (apx_sd_init()) break; } run_suite(); }
            printf("done; remove the card\n");
        }
        /* wait for removal: a read of the MBR every 500 ms */
        for (;;) {
            idle_wait(TICKS / 2);
            if (apx_sd_read(0, back, 1)) { printf("card removed\n"); break; }
        }
    }
}

/*--------------------------------------------------------------------+
 * Mode 1: hot-pull
 *--------------------------------------------------------------------*/
enum { P_IDLE, P_READ, P_WRITE, P_COUNT };
static const char phase_name[] = "IRW";

__attribute__((unused)) static void hotpull_mode(void) {
    int r = apx_sd_init();
    printf("hotpull: init %d\n", r);
    if (r) { for (;;) apx_debug_poll(); }
    char name0[8];
    uint32_t serial0;
    card_id(name0, &serial0);
    const unsigned width0 = apx_sd_get_info()->bus_width;
    const unsigned gap = find_gap();
    if (gap < 8) { printf("hotpull: no usable gap (%u): nothing written\n", gap); for (;;) apx_debug_poll(); }
    printf("hotpull: card %s %08lx, gap %u sectors, %u s; pull and reinsert the card, as often as you like\n", name0, (unsigned long)serial0, gap, APX_SCT_SECONDS);
    const uint32_t end = apx_ticks() + TICKS * APX_SCT_SECONDS;
    unsigned phase = P_IDLE, events = 0, recovered = 0, other_card = 0, narrow = 0; /* narrow: recoveries that came back on one data line */
    uint32_t phase_until = apx_ticks() + TICKS * 5, generation = 0, max_call = 0, last_ok = apx_ticks(), reads = 0, writes = 0;
    uint32_t read_lba = 64;
    while (!apx_deadline_expired(end)) {
        const uint32_t t0 = apx_ticks();
        int err = 0;
        if (phase == P_IDLE) { err = apx_sd_read(0, back, 1); idle_wait(TICKS / 10); }
        else if (phase == P_READ) { err = apx_sd_read(read_lba, back, 8); read_lba = read_lba + 8 < 4096 ? read_lba + 8 : 64; ++reads; }
        else { /* a generation: every gap sector written one by one (a CMD25 stream), then synced */
            fill(buf, 70 + generation, 512u, 512u);
            for (unsigned s = 1; s <= gap && !err; ++s) { fill(buf, 70 + generation, (uint64_t)s * 512u, 512u); err = apx_sd_write(s, buf, 1); }
            if (!err) err = apx_sd_sync();
            if (!err) ++generation;
            ++writes;
        }
        const uint32_t dt = apx_ticks() - t0;
        if (dt > max_call) max_call = dt;
        if (!err) {
            last_ok = apx_ticks();
            if (apx_deadline_expired(phase_until)) { phase = (phase + 1) % P_COUNT; phase_until = apx_ticks() + TICKS * 5; }
            continue;
        }
        /* the card is gone (or failing): log it, then wait for it to come back */
        ++events;
        const struct apx_sd_info *info = apx_sd_get_info();
        const uint32_t fail_at = apx_ticks();
        printf("EVT %u phase %c err %d cmd %lu st %08lx call %lu ms since-ok %lu ms init %u\n", events, phase_name[phase], err, (unsigned long)info->last_failure_command ? (unsigned long)info->last_failure_command : (unsigned long)info->last_command,
               (unsigned long)info->last_status, ms(dt), ms(fail_at - last_ok), info->initialized);
        uint32_t tries = 0;
        for (;;) {
            if (apx_deadline_expired(end)) break;
            idle_wait(TICKS / 3);
            ++tries;
            if (apx_sd_init() == 0) {
                char n[8];
                uint32_t s;
                card_id(n, &s);
                if (s == serial0 && !memcmp(n, name0, 8)) { ++recovered; printf("  back after %lu ms (%lu init tries) bus %u probe %u\n", ms(apx_ticks() - fail_at), (unsigned long)tries, apx_sd_get_info()->bus_width, apx_sd_get_info()->bus_probe); if (apx_sd_get_info()->bus_width == 1) ++narrow; break; }
                ++other_card;
                printf("  a different card %s %08lx is in: ignored\n", n, (unsigned long)s);
            }
        }
        last_ok = apx_ticks();
        phase_until = apx_ticks() + TICKS * 5;
    }
    /* what the card holds now */
    printf("hotpull: bus width at start %u; recoveries on ONE line: %u\n", width0, narrow);
    printf("hotpull: end. events %u, recovered %u, other cards %u, write generations %lu (%lu started), reads %lu, longest call %lu ms\n", events, recovered, other_card, (unsigned long)generation,
           (unsigned long)writes, (unsigned long)reads, ms(max_call));
    if (apx_sd_init() == 0) {
        memset(back, 0, gap * 512u);
        unsigned good[4] = { 0, 0, 0, 0 }, torn = 0, original = 0;
        if (apx_sd_read(1, back, gap) == 0) {
            for (unsigned s = 0; s < gap; ++s) {
                int matched = 0;
                for (unsigned g = 0; g < 3 && !matched; ++g) {
                    if (generation < g) break;
                    if (same(back + s * 512, 70 + generation - g, (uint64_t)(s + 1) * 512u, 512u)) { ++good[g]; matched = 1; }
                }
                if (!matched) { if (!memcmp(back + s * 512, snapshot + s * 512, 512)) ++original; else ++torn; }
            }
            printf("hotpull: gap sectors by generation: latest %u, -1 %u, -2 %u, original %u, TORN/other %u\n", good[0], good[1], good[2], original, torn);
        } else printf("hotpull: cannot read the gap back\n");
        int ok = apx_sd_write(1, snapshot, gap) == 0 && apx_sd_sync() == 0;
        memset(back, 0x5a, gap * 512u);
        ok = ok && apx_sd_read(1, back, gap) == 0 && !memcmp(back, snapshot, gap * 512u);
        printf("hotpull: gap restored and verified: %s\n", ok ? "yes" : "NO");
    } else printf("hotpull: card not present at the end\n");
    for (;;) apx_debug_poll();
}

/* Mode 2: a closer look at a failing card: a raw write of sector 1 (and of 1..2 as a stream), what comes back, with the controller's view of each step. */
__attribute__((unused)) static void diag_mode(void) {
    for (;;) {
        int r = apx_sd_init();
        printf("diag: init %d\n", r);
        if (r) { idle_wait(TICKS * 2); continue; }
        unsigned gap = find_gap();
        printf("diag: gap %u\n", gap);
        {   /* what is in sectors 1..62 now */
            char nm[8]; uint32_t ser; card_id(nm, &ser);
            if (apx_sd_read(1, back, 62)) printf("diag: cannot read 1..62\n");
            else {
                unsigned nz = 0;
                for (unsigned sct = 0; sct < 62; ++sct) {
                    unsigned c = 0; for (unsigned i = 0; i < 512; ++i) if (back[sct * 512 + i]) ++c;
                    if (c) { ++nz; if (nz <= 6) printf("diag: sector %u: %u nonzero bytes, first %02x %02x %02x %02x\n", 1 + sct, c, back[sct * 512], back[sct * 512 + 1], back[sct * 512 + 2], back[sct * 512 + 3]); }
                }
                printf("diag: %u of 62 sectors nonzero\n", nz);
                if (nz && ser == 0x27647cc1u) { /* this card's gap was 62 zero sectors before the failed run (logged): put that back */
                    memset(buf, 0, 62 * 512);
                    int w = apx_sd_write(1, buf, 62), sy = apx_sd_sync();
                    memset(back, 0xee, 62 * 512);
                    int rd = apx_sd_read(1, back, 62);
                    unsigned z = 0; for (unsigned i = 0; i < 62 * 512; ++i) z += back[i] == 0;
                    printf("diag: zero-fill write %d sync %d read %d zero bytes %u of %u\n", w, sy, rd, z, 62 * 512);
                    gap = find_gap();
                }
            }
        }
        if (gap < 4) { for (;;) apx_debug_poll(); }
        const struct apx_sd_info *info = apx_sd_get_info();
        printf("diag: hc %u clock %lu sectors %lu\n", info->high_capacity, (unsigned long)info->clock_hz, (unsigned long)info->sectors);
        for (unsigned round = 0; round < 4; ++round) {
            const unsigned count = round < 2 ? 1 : 2;
            fill(buf, 90 + round, 512u, count * 512u);
            int w = apx_sd_write(1, buf, count);
            const uint32_t st = apx_sd_get_info()->last_write_status;
            int sy = apx_sd_sync();
            memset(back, 0xee, 1024);
            int rd = apx_sd_read(1, back, count);
            unsigned wrong = 0, first_bad = 0;
            for (unsigned i = 0; i < count * 512u; ++i) if (back[i] != buf[i]) { if (!wrong) first_bad = i; ++wrong; }
            printf("diag: r%u count %u write %d status %08lx sync %d read %d wrong %u first_bad %u want %02x got %02x | back[0..3] %02x %02x %02x %02x want %02x %02x %02x %02x\n", round, count, w, (unsigned long)st, sy, rd, wrong, first_bad,
                   buf[first_bad], back[first_bad], back[0], back[1], back[2], back[3], buf[0], buf[1], buf[2], buf[3]);
            idle_wait(TICKS);
            memset(back, 0xee, 1024);
            rd = apx_sd_read(1, back, count);
            wrong = 0;
            for (unsigned i = 0; i < count * 512u; ++i) if (back[i] != buf[i]) ++wrong;
            printf("diag:   re-read after 1 s: read %d wrong %u\n", rd, wrong);
        }
        /* a different LBA range for comparison: a high gap sector pair if there is one */
        int w = apx_sd_write(1, snapshot, gap);
        apx_sd_sync();
        memset(back, 0xee, gap * 512u);
        int rd = apx_sd_read(1, back, gap);
        printf("diag: restore write %d read %d identical %d\n", w, rd, !memcmp(back, snapshot, gap * 512u));
        printf("diag: done\n");
        for (;;) apx_debug_poll();
    }
}

/* Mode 3: look at the gap after a hot-pull run that ended with the card out (APX_SCT_GEN = the run's last generation count), then put it back to zeros
 * (APX_SCT_RESTORE_SERIAL: only for the card whose gap was logged as all zero before the run). */
#ifndef APX_SCT_GEN
#define APX_SCT_GEN 0
#endif
#ifndef APX_SCT_RESTORE_SERIAL
#define APX_SCT_RESTORE_SERIAL 0
#endif
__attribute__((unused)) static void gapcheck_mode(void) {
    for (;;) {
        if (apx_sd_init()) { idle_wait(TICKS); continue; }
        char nm[8]; uint32_t ser; card_id(nm, &ser);
        printf("gapcheck: card %s %08lx\n", nm, (unsigned long)ser);
        memset(back, 0, 62 * 512);
        if (apx_sd_read(1, back, 62)) { printf("gapcheck: read failed\n"); idle_wait(TICKS); continue; }
        unsigned cat[6] = { 0, 0, 0, 0, 0, 0 }; /* gen, gen-1, gen-2, gen-3, zero, other */
        for (unsigned sct = 0; sct < 62; ++sct) {
            int matched = 0;
            for (unsigned g = 0; g < 4 && !matched; ++g) if (same(back + sct * 512, 70 + APX_SCT_GEN - g, (uint64_t)(sct + 1) * 512u, 512u)) { ++cat[g]; matched = 1; }
            if (!matched) { int zero = 1; for (unsigned i = 0; i < 512; ++i) if (back[sct * 512 + i]) { zero = 0; break; } if (zero) ++cat[4]; else { ++cat[5]; if (cat[5] <= 4) printf("gapcheck: sector %u is neither a known generation nor zero: %02x %02x %02x %02x\n", 1 + sct, back[sct * 512], back[sct * 512 + 1], back[sct * 512 + 2], back[sct * 512 + 3]); } }
        }
        printf("gapcheck: sectors by generation: %u (in progress), %u, %u, %u (older), zero %u, TORN/other %u\n", cat[0], cat[1], cat[2], cat[3], cat[4], cat[5]);
        if (ser == APX_SCT_RESTORE_SERIAL) {
            memset(buf, 0, 62 * 512);
            int w = apx_sd_write(1, buf, 62), sy = apx_sd_sync();
            memset(back, 0xee, 62 * 512);
            int rd = apx_sd_read(1, back, 62);
            unsigned z = 0; for (unsigned i = 0; i < 62 * 512; ++i) z += back[i] == 0;
            printf("gapcheck: zero-fill write %d sync %d read %d zero bytes %u of %u: %s\n", w, sy, rd, z, 62 * 512, z == 62 * 512 ? "restored" : "NOT restored");
        }
        printf("gapcheck: done\n");
        for (;;) apx_debug_poll();
    }
}

int main(void) {
    printf("sd-card-test mode %d\n", APX_SCT_MODE);
    apx_usb_init();
#if APX_SCT_MODE == 1
    hotpull_mode();
#elif APX_SCT_MODE == 2
    diag_mode();
#elif APX_SCT_MODE == 3
    gapcheck_mode();
#else
    cards_mode();
#endif
}
