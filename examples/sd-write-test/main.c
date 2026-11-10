/* SD write acceptance and benchmark. WRITES TO THE INSERTED CARD (the owner approved it for the card in the Silvergear, 2026-11-08).
 *
 * What it touches, and how it puts it back:
 *   - the gap between the MBR and the first partition (sectors 1..first_partition-1, at most 62 of them): saved first, used for the raw tests, restored and
 *     verified at the end. It refuses to run unless the partition table shows such a gap, and never writes outside it.
 *   - one scratch file, 0:/APXWTEST.BIN (up to 8 MiB), created, verified by reading it back, and deleted. Nothing else on the card is written, except what the
 *     filesystem itself updates for that file (FAT, directory entry).
 *
 * Raw tests: single sectors (CMD24), multi-sector writes (CMD25), sequential single sectors (stream continuation), an unaligned source buffer, reads between
 * writes, and the data-response token of every write. File tests: 512-byte, 4 KiB and 64 KiB write sizes through FatFs with the rate of each, read-back and
 * verification of the whole file.
 *
 * Results: one short log line per step ("PASS"/"FAIL ...") and the struct `sd_write_test` (read it with apxtool debug read). */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/sd.h>
#include <appotech/storage.h>
#include <stdio.h>
#include <string.h>

#ifndef APX_WT_START_SECONDS
#define APX_WT_START_SECONDS 6 /* time in debug mode before anything is written (time to take an external backup over the debug link) */
#endif
#ifndef APX_WT_FILE_MIB
#define APX_WT_FILE_MIB 8
#endif
#define BUF_BYTES 65536u
static uint8_t buf[BUF_BYTES + 64] __attribute__((section(".large_buffer"), aligned(32)));
static uint8_t back[BUF_BYTES + 64] __attribute__((section(".large_buffer"), aligned(32)));
static uint8_t snapshot[62 * 512] __attribute__((section(".large_buffer"), aligned(32)));

struct { uint32_t magic, done, steps, failures, gap_first, gap_last, errors, retries, writes, wstreams; uint32_t kbps[3]; } sd_write_test __attribute__((used));

static unsigned steps, failures;
static void result(int ok, const char *what) {
    ++steps;
    if (!ok) ++failures;
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

/* byte i of the data stream, position dependent (seed separates tests) */
static uint8_t pattern(uint32_t seed, uint64_t offset) { return (uint8_t)((offset >> 9) * 131u + (offset & 511u) * 7u + seed * 29u + 1u); }
static void fill(uint8_t *p, uint32_t seed, uint64_t offset, size_t n) { for (size_t i = 0; i < n; ++i) p[i] = pattern(seed, offset + i); }
static int same(const uint8_t *p, uint32_t seed, uint64_t offset, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i] != pattern(seed, offset + i)) return 0; return 1; }

static int token_ok(void) { return (apx_sd_get_info()->last_write_status & 0x600000u) == 0x400000u; }

/* write `count` sectors at `lba` from `source` (pattern seed), sync, read back through the driver, compare */
static int write_and_verify(uint32_t lba, const uint8_t *source, unsigned count, uint32_t seed, int sync) {
    if (apx_sd_write(lba, source, count)) return 0;
    if (!token_ok()) return 0;
    if (sync && apx_sd_sync()) return 0;
    memset(back, 0, count * 512u);
    if (apx_sd_read(lba, back, count)) return 0;
    return same(back, seed, (uint64_t)lba * 512u, count * 512u);
}

static void raw_tests(unsigned gap) {
    char text[64];
    static const unsigned sizes[] = { 1, 2, 3, 7, 16, 33 };
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; ++k) {
        unsigned n = sizes[k] <= gap ? sizes[k] : gap;
        fill(buf, 10 + k, 512u, n * 512u);
        snprintf(text, sizeof text, "raw %u sector(s) at LBA 1, synced", n);
        result(write_and_verify(1, buf, n, 10 + k, 1), text);
    }
    /* the same without a sync before the read (the read closes the write stream itself) */
    fill(buf, 20, 512u, 8 * 512u);
    result(write_and_verify(1, buf, 8, 20, 0), "raw 8 sectors, read-back without sync");
    /* an unaligned source buffer goes through the bounce block */
    fill(buf + 1, 21, 512u, 5 * 512u);
    result(write_and_verify(1, buf + 1, 5, 21, 1), "raw 5 sectors from an unaligned buffer");
    /* single sectors one after the other: the second starts a stream, the rest continue it */
    {
        int ok = 1;
        for (unsigned s = 1; s <= gap && ok; ++s) {
            fill(buf, 30, (uint64_t)s * 512u, 512u);
            ok = apx_sd_write(s, buf, 1) == 0 && token_ok();
        }
        ok = ok && apx_sd_sync() == 0;
        memset(back, 0, gap * 512u);
        ok = ok && apx_sd_read(1, back, gap) == 0 && same(back, 30, 512u, gap * 512u);
        result(ok, "raw sequential single-sector writes (stream continuation)");
    }
    /* interleaved: write, read somewhere else, write the next sector (the read closes the stream; the write after it must start again) */
    {
        int ok = 1;
        for (unsigned round = 0; round < 6 && ok; ++round) {
            fill(buf, 40 + round, (uint64_t)(1 + round) * 512u, 2 * 512u);
            ok = apx_sd_write(1 + round, buf, 2) == 0 && apx_sd_read(0, back + 4096, 1) == 0; /* the MBR, untouched */
        }
        ok = ok && apx_sd_sync() == 0 && back[4096 + 510] == 0x55 && back[4096 + 511] == 0xaa;
        result(ok, "raw writes interleaved with reads (MBR intact)");
    }
    const struct apx_sd_info *info = apx_sd_get_info();
    result(info->errors == 0 && info->retries == 0, "no errors and no retries during the raw tests");
}

static int file_tests(void) {
    FIL file;
    char text[64];
    DWORD free_clusters;
    FATFS *fs;
    if (apx_storage_mount() != FR_OK) { result(0, "mount"); return 0; }
    result(1, "mount");
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK || (uint64_t)free_clusters * fs->csize * 512u < (uint64_t)APX_WT_FILE_MIB * 1048576u + 1048576u) { result(0, "enough free space for the test file"); return 0; }
    if (f_open(&file, "0:/APXWTEST.BIN", FA_CREATE_NEW | FA_WRITE | FA_READ) != FR_OK) { result(0, "create 0:/APXWTEST.BIN (it must not exist)"); return 0; }
    static const struct { unsigned chunk, mib; const char *label; } phases[3] = { { 512, 1, "512 B" }, { 4096, 1, "4 KiB" }, { 65536, APX_WT_FILE_MIB - 2, "64 KiB" } };
    uint64_t offset = 0;
    for (unsigned p = 0; p < 3; ++p) {
        const uint32_t total = phases[p].mib * 1048576u;
        const uint32_t t0 = apx_ticks();
        int ok = 1;
        for (uint32_t done = 0; done < total && ok; done += phases[p].chunk) {
            fill(buf, 50, offset + done, phases[p].chunk);
            UINT written = 0;
            ok = f_write(&file, buf, phases[p].chunk, &written) == FR_OK && written == phases[p].chunk;
        }
        ok = ok && f_sync(&file) == FR_OK;
        const uint32_t dt = apx_ticks() - t0;
        offset += total;
        const unsigned long kbps = dt ? (unsigned long)((uint64_t)total / 1024u * 32768u / dt) : 0;
        sd_write_test.kbps[p] = (uint32_t)kbps;
        snprintf(text, sizeof text, "file write %u MiB in %s chunks: %lu KB/s", phases[p].mib, phases[p].label, kbps);
        result(ok, text);
    }
    result(f_size(&file) == offset && f_close(&file) == FR_OK, "file size and close");
    /* read everything back and compare */
    if (f_open(&file, "0:/APXWTEST.BIN", FA_READ) != FR_OK) { result(0, "reopen for reading"); return 0; }
    {
        uint32_t t0 = apx_ticks();
        int ok = 1;
        for (uint64_t at = 0; at < offset && ok; at += BUF_BYTES) {
            UINT got = 0;
            ok = f_read(&file, back, BUF_BYTES, &got) == FR_OK && got == BUF_BYTES && same(back, 50, at, BUF_BYTES);
        }
        const uint32_t dt = apx_ticks() - t0;
        snprintf(text, sizeof text, "file read back and verified: %lu KB/s", dt ? (unsigned long)(offset / 1024u * 32768u / dt) : 0ul);
        result(ok, text);
        f_close(&file);
    }
    result(f_unlink("0:/APXWTEST.BIN") == FR_OK, "delete the test file");
    FILINFO info;
    result(f_stat("0:/APXWTEST.BIN", &info) == FR_NO_FILE, "the file is gone");
    result(apx_storage_sync() == FR_OK, "filesystem sync");
    return 1;
}

int main(void) {
    printf("sd-write-test starting\n");
    apx_usb_init();
    sd_write_test.magic = 0x57545354;
    int init = apx_sd_init(); /* first, so that the debug link can read the card (apxtool debug sd read-sector) during the wait: take the backup then */
    uint32_t until = apx_ticks() + 32768u * APX_WT_START_SECONDS;
    while (!apx_deadline_expired(until)) apx_debug_poll();
    const struct apx_sd_info *info = apx_sd_get_info();
    printf("sd init %d, %lu sectors, high speed %u, %lu Hz\n", init, (unsigned long)info->sectors, info->high_speed, (unsigned long)info->clock_hz);
    if (init || info->write_protected) { printf("no usable writable card\n"); sd_write_test.done = 2; for (;;) apx_debug_poll(); }

    /* the gap between the MBR and the first partition: the only raw area this test writes */
    if (apx_sd_read(0, buf, 1)) { printf("cannot read the MBR\n"); sd_write_test.done = 2; for (;;) apx_debug_poll(); }
    const uint32_t first = buf[454] | buf[455] << 8 | buf[456] << 16 | (uint32_t)buf[457] << 24;
    const int mbr = buf[510] == 0x55 && buf[511] == 0xaa;
    if (!mbr || first < 2 || first > 63) { printf("no MBR gap to test in (partition at %lu): raw tests skipped, nothing written\n", (unsigned long)first); sd_write_test.done = 3; for (;;) apx_debug_poll(); }
    const unsigned gap = first - 1; /* sectors 1..gap */
    sd_write_test.gap_first = 1;
    sd_write_test.gap_last = gap;
    printf("gap: sectors 1..%u\n", gap);
    if (apx_sd_read(1, snapshot, gap)) { printf("cannot read the gap\n"); sd_write_test.done = 2; for (;;) apx_debug_poll(); }

    raw_tests(gap);

    /* put the gap back exactly as it was */
    {
        int ok = apx_sd_write(1, snapshot, gap) == 0 && apx_sd_sync() == 0;
        memset(back, 0x5a, gap * 512u);
        ok = ok && apx_sd_read(1, back, gap) == 0 && memcmp(back, snapshot, gap * 512u) == 0;
        result(ok, "gap restored and verified against the snapshot");
    }
    file_tests();

    info = apx_sd_get_info();
    sd_write_test.errors = info->errors;
    sd_write_test.retries = info->retries;
    sd_write_test.writes = info->writes;
    sd_write_test.wstreams = info->wstreams;
    result(info->errors == 0 && info->retries == 0, "no errors and no retries in the whole run");
    printf("sd-write-test: %u steps, %u failures; writes %lu, write streams %lu, errors %lu, retries %lu\n", steps, failures, (unsigned long)info->writes,
           (unsigned long)info->wstreams, (unsigned long)info->errors, (unsigned long)info->retries);
    sd_write_test.steps = steps;
    sd_write_test.failures = failures;
    sd_write_test.done = 1;
    for (;;) apx_debug_poll();
}
