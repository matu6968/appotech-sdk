/* Does the microphone DMA expose its progress? Starts one 2-second capture at 32 kHz (a 64 KiB buffer) and, while it runs, reads SPR 0x9108 (destination), 0x910c (length), 0x9114
 * (status), and checks how far into the buffer non-zero samples have appeared. Also reports the level of the finished capture. Read-only (no SD, no USB mode change). */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/microphone.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


int main(void) {
    printf("mic-probe starting\n");
    apx_usb_init();
    uint32_t until = apx_ticks() + 32768u * 6;
    while (!apx_deadline_expired(until)) apx_debug_poll();
    int e = apx_mic_init();
    printf("mic init %d\n", e);
    /* Experiment 3: the cost of restarting. Back-to-back 16 KiB captures (256 ms each): wall time per chunk against the 256 ms of audio, and the time spent inside the start call. */
    static int16_t chunk[2][8192] __attribute__((section(".large_buffer"), aligned(32)));
    unsigned which = 0;
    e = apx_mic_capture_start(chunk[0], sizeof chunk[0], 32000);
    uint32_t t_first = apx_ticks(), call_total = 0, call_max = 0;
    const unsigned N = 12;
    for (unsigned n = 0; n < N; ++n) {
        for (;;) { apx_mic_poll(); struct apx_mic_info mi; apx_mic_get_info(&mi); if (mi.state != APX_MIC_CAPTURING) break; }
        const uint32_t t_done = apx_ticks();
        which ^= 1u;
        const uint32_t t_call = apx_ticks();
        e = apx_mic_capture_start(chunk[which], sizeof chunk[0], 32000);
        const uint32_t dt = apx_ticks() - t_call;
        call_total += dt; if (dt > call_max) call_max = dt;
        (void)t_done;
    }
    const uint32_t total = apx_ticks() - t_first;
    printf("%u chunks of 256 ms: wall %lu ms (%lu ms of audio), so about %lu ms lost per restart; start call avg %lu max %lu ticks of 30 us, last start %d\n", N, (unsigned long)(total * 1000ull / 32768u),
           (unsigned long)(N * 256u), (unsigned long)((total * 1000ull / 32768u - N * 256u) / N), (unsigned long)(call_total / N), (unsigned long)call_max, e);
    apx_mic_stop();
    /* Experiment 4: continuous capture with the next buffer prepared in advance (what the UVC example does): the DMA of one 32768-sample buffer takes 33555 ticks of 32.768 kHz
     * (32,000.3 Hz) on the Silvergear; the handover between buffers is apx_mic_rearm_prepared(), registers only. Reports ticks per buffer, the handover latency, and whether the
     * samples are real (a sentinel-filled buffer that the DMA overwrote). */
    {
        enum { SAMPLES = 32768, BUFFERS = 6 };
        static int16_t cb[2][SAMPLES] __attribute__((section(".large_buffer"), aligned(32)));
        for (unsigned b = 0; b < 2; ++b) { for (unsigned i = 0; i < SAMPLES; ++i) cb[b][i] = -32768; apx_dma_clean(cb[b], sizeof cb[b]); apx_dma_invalidate(cb[b], sizeof cb[b]); }
        e = apx_mic_capture_start_continuous(cb[0], sizeof cb[0], 32000);
        unsigned cur = 0, done = 0, errors = 0;
        uint32_t last_start = apx_ticks(), lat_sum = 0, lat_max = 0, dma_sum = 0;
        while (done < BUFFERS && e == 0) {
            apx_maintenance(); /* services the watchdog and polls the microphone */
            struct apx_mic_info mi;
            apx_mic_get_info(&mi);
            if (mi.state == APX_MIC_CAPTURING) continue;
            if (mi.state != APX_MIC_DONE) { ++errors; break; }
            const unsigned next = cur ^ 1u;
            if (apx_mic_rearm_prepared(cb[next], sizeof cb[next])) { ++errors; break; }
            const uint32_t now = apx_ticks();
            if (done) { const uint32_t lat = now - last_start - mi.ticks; lat_sum += lat; if (lat > lat_max) lat_max = lat; dma_sum += mi.ticks; }
            last_start = now;
            unsigned sentinel = 0;
            apx_dma_invalidate(cb[cur], sizeof cb[cur]);
            for (unsigned i = 0; i < SAMPLES; ++i) sentinel += cb[cur][i] == -32768;
            printf("  buffer %u: dma %lu ticks, %u samples still the sentinel\n", done, (unsigned long)mi.ticks, sentinel);
            for (unsigned i = 0; i < SAMPLES; ++i) cb[cur][i] = -32768; /* ready the buffer for its next turn */
            apx_dma_clean(cb[cur], sizeof cb[cur]); apx_dma_invalidate(cb[cur], sizeof cb[cur]);
            cur = next; ++done;
        }
        apx_mic_stop();
        printf("continuous: %u buffers, errors %u, start %d; mean dma %lu ticks per buffer (%lu Hz), handover latency avg %lu max %lu ticks\n", done, errors, e,
               (unsigned long)(done > 1 ? dma_sum / (done - 1) : 0), (unsigned long)(done > 1 ? (uint64_t)SAMPLES * 32768u / (dma_sum / (done - 1)) : 0),
               (unsigned long)(done > 1 ? lat_sum / (done - 1) : 0), (unsigned long)lat_max);
    }
    printf("mic-probe done\n");
    for (;;) apx_debug_poll();
}
