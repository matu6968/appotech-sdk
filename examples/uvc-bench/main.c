/* Throughput bench of the UVC bulk video path: no camera, no JPEG, no SD. Each video stream the host opens is fed a fixed buffer (APX_BENCH_BYTES) as fast as the USB
 * stack takes it, and the rate is printed every 5 s; the host side is just a client that reads and discards frames (v4l2-ctl --stream-to=/dev/null).
 *
 *   APX_BENCH_BYTES    frame size (default 100000)
 *   APX_BENCH_SECONDS  time in camera mode (default 60), then back to debug mode so the log can be read
 * Mass storage is the RAM disk. */
#include <appotech/soc.h>
#include "board.h"
#include <appotech/usb.h>
#include <appotech/usb_msc.h>
#include <appotech/usb_uvc.h>
#include <appotech/debug.h>
#include <stdio.h>

#ifndef APX_BENCH_BYTES
#define APX_BENCH_BYTES 100000u
#endif
#ifndef APX_BENCH_SECONDS
#define APX_BENCH_SECONDS 60
#endif
#define TICKS_PER_SECOND 32768u
#define LEAD 32u /* the frame starts at a 32-byte offset: the two bytes before it are the payload header's */

static uint8_t frame_buf[LEAD + APX_BENCH_BYTES + 64] __attribute__((section(".large_buffer"), aligned(32)));
#define frame (frame_buf + LEAD)

static void fill(void) { /* a pattern the host can check: byte i = i * 7 + (i >> 8), SOI and EOI at the ends */
    for (unsigned i = 0; i < APX_BENCH_BYTES + 64; ++i) frame[i] = (uint8_t)(i * 7u + (i >> 8));
    frame[0] = 0xff; frame[1] = 0xd8; frame[APX_BENCH_BYTES - 2] = 0xff; frame[APX_BENCH_BYTES - 1] = 0xd9;
}

int main(void) {
    printf("uvc-bench starting, %u byte frames\n", (unsigned)APX_BENCH_BYTES);
    apx_usb_init();
    fill();
    uint32_t until = apx_ticks() + TICKS_PER_SECOND * 8;
    while (!apx_deadline_expired(until)) apx_debug_poll();
    apx_usb_msc_set_disk(NULL);
    if (apx_usb_set_mode(APX_USB_MODE_CAMERA) != 0) {
        printf("camera mode unavailable\n");
        for (;;) apx_debug_poll();
    }
    printf("camera mode\n");
    const uint32_t end = apx_ticks() + TICKS_PER_SECOND * APX_BENCH_SECONDS;
    uint32_t report = apx_ticks() + TICKS_PER_SECOND * 5, t0 = apx_ticks(), sent0[2] = { 0, 0 }, loops = 0;
    while (!apx_deadline_expired(end)) {
        ++loops;
        apx_usb_poll();
        for (unsigned s = 0; s < APX_UVC_STREAMS; ++s)
            if (apx_usb_uvc_streaming(s) && !apx_usb_uvc_busy(s)) {
#ifdef APX_BENCH_VERIFY /* sending overwrites two bytes in front of every payload after the first: put the pattern back so the host can check the frame */
                fill();
#endif
                (void)apx_usb_uvc_send_frame(s, frame, APX_BENCH_BYTES);
            }
        if (apx_deadline_expired(report)) {
            report += TICKS_PER_SECOND * 5;
            const uint32_t now = apx_ticks();
            for (unsigned s = 0; s < APX_UVC_STREAMS; ++s) {
                const uint32_t n = apx_usb_uvc_frames_sent(s) - sent0[s];
                sent0[s] = apx_usb_uvc_frames_sent(s);
                if (n) printf("stream %u: %lu frames in %lu ms = %lu KB/s\n", s, (unsigned long)n, (unsigned long)((uint64_t)(now - t0) * 1000u / TICKS_PER_SECOND),
                              (unsigned long)((uint64_t)n * APX_BENCH_BYTES * TICKS_PER_SECOND / 1024u / (now - t0)));
            }
            printf("loop: %lu polls/s\n", (unsigned long)(loops / 5));
            loops = 0;
            t0 = now;
        }
    }
    printf("uvc-bench: back to debug mode\n");
    apx_usb_set_mode(APX_USB_MODE_DEBUG);
    for (;;) apx_debug_poll();
}
