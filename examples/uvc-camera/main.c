/* UVC camera over USB: the board's sensors as MJPEG webcams (hardware JPEG encoder), next to the SD card as a read-only mass-storage disk, in one composite device like the
 * stock firmware's (APX_USB_MODE_CAMERA). The Silvergear has two sensors and shows two video devices: the host picks the camera by opening one of them (stream 0 is the REAR
 * sensor, stream 1 the FRONT one, "JT5295 Rear Camera" / "JT5295 Front Camera"); if both are opened, the sensors are served in turn, one second each (a sensor switch costs
 * time). The Action Vision has one sensor and shows one video device, "JT5295 Camera", like most webcams.
 *
 *   APX_UVC_DEBUG_SECONDS  time in debug mode first (default 8): enough to read the log or let a tool attach; the camera mode has no APXD link
 *   APX_UVC_SECONDS        time in camera mode (default 90), then back to debug mode so the log can be read; 0 = stay
 *   APX_UVC_SD             1 = present the SD card read-only (default), 0 = the RAM disk
 *
 * Three stages run side by side, cooperatively in one loop, so the sensor, the JPEG engine and the USB link are all busy at once:
 *   capture  the sensor writes a scaled NV21 frame (320x240 or 640x480, as the host committed) into one of two frame buffers;
 *   encode   the JPEG engine turns the other frame buffer into one of two JPEG buffers;
 *   send     a finished JPEG goes out on its stream while the next ones are being made. */
#include <appotech/soc.h>
#include "board.h"
#include <appotech/usb.h>
#include <appotech/usb_msc.h>
#include <appotech/usb_uvc.h>
#include <appotech/debug.h>
#include <appotech/camera.h>
#include <appotech/jpeg.h>
#if APX_USB_UAC
#include <appotech/usb_uac.h>
#include <appotech/microphone.h>
#endif
#include <stdio.h>
#include <string.h>

#ifndef APX_UVC_DEBUG_SECONDS
#define APX_UVC_DEBUG_SECONDS 8
#endif
#ifndef APX_UVC_SECONDS
#define APX_UVC_SECONDS 90
#endif
#ifndef APX_UVC_SD
#define APX_UVC_SD 1
#endif
#define JPEG_CAPACITY (128u * 1024u)
#define JPEG_LEAD 32u /* the JPEG starts this far into its buffer: sending writes the UVC payload header in the bytes before it */
#define FRAME_CAPACITY (APX_CAMERA_FRAME_BYTES + 64u)
#define TICKS_PER_SECOND 32768u

/* One frame buffer in the low application window, one next to the JPEG buffers in codec RAM (neither fits twice in either). */
static uint8_t frame_low[FRAME_CAPACITY] __attribute__((section(".large_buffer"), aligned(32)));
static uint8_t frame_codec[FRAME_CAPACITY] __attribute__((section(".jpeg_buffer"), aligned(32)));
static uint8_t jpeg_buffer[2][JPEG_CAPACITY] __attribute__((section(".jpeg_buffer"), aligned(32)));

enum { NV_FREE, NV_CAPTURING, NV_FULL, NV_ENCODING };
enum { J_FREE, J_ENCODING, J_READY, J_SENDING };
static struct { uint8_t *buf; unsigned state, stream, index, w, h; } nv[2] = { { .buf = frame_low }, { .buf = frame_codec } };
static struct { uint8_t *buf; unsigned state, stream; uint32_t bytes, seq, sent_at; } js[2] = { { .buf = jpeg_buffer[0] + JPEG_LEAD }, { .buf = jpeg_buffer[1] + JPEG_LEAD } };
static unsigned cap_i, enc_i;
static uint32_t seq_next;

static struct {
    uint32_t captured[2], encoded[2], queued[2], capture_errors, encode_errors, dropped, switches, paced_out, send_ticks, send_bytes, send_frames;
    uint32_t last_bytes[2], capture_ticks, encode_ticks, select_ticks, started, start_call_ticks, poll_done_ticks, encode_call_ticks;
} stat;

/* Streams the host has stopped reading: bulk streaming has no stop signal, so a stream is taken to be abandoned when frames were offered to it AFTER the last frame that went out
 * completely, and none has gone out for a second (twice the frame interval when that is longer). Serving it would be wasted work (and power) and, with both sensors in use, keeps
 * switching the sensor. The count of offers is taken at the moment of each success, so that:
 *   - a stream whose last frame went out and that is merely waiting for its turn on the sensor has no offer outstanding and is never idle (an earlier version counted "a frame was
 *     offered" even when that frame had already gone out, and a live stream was declared abandoned during the other camera's turn and, being idle, never served again);
 *   - the stale frame a restarted stream still holds from its previous session (it was offered before the new session began) does not count against the new session (a version
 *     that timed "a frame is stuck" did that, and declared a restarted stream idle before it had been given a chance).
 * Progress (a frame completely sent) or a new session ends the idle state. */
static unsigned host_idle[2];
static uint32_t offered_since[2], seen_sent[2], seen_session[2], offers_at_progress[2];
static void watch_host(void) {
    for (unsigned s = 0; s < APX_UVC_STREAMS; ++s) {
        const uint32_t sent = apx_usb_uvc_frames_sent(s), session = apx_usb_uvc_session(s);
        if (sent != seen_sent[s] || session != seen_session[s]) {
            seen_sent[s] = sent;
            seen_session[s] = session;
            offers_at_progress[s] = stat.queued[s];
            offered_since[s] = 0;
            host_idle[s] = 0;
        }
        if (stat.queued[s] == offers_at_progress[s]) offered_since[s] = 0; /* nothing offered since the last success */
        else if (!offered_since[s]) offered_since[s] = apx_ticks() | 1u;
        uint32_t limit = (uint32_t)((uint64_t)apx_usb_uvc_frame_interval_us(s) * 2u * TICKS_PER_SECOND / 1000000u);
        if (limit < TICKS_PER_SECOND) limit = TICKS_PER_SECOND;
        if (offered_since[s] && apx_ticks() - offered_since[s] > limit) host_idle[s] = 1;
    }
}

static unsigned wanted(unsigned s) { return apx_usb_uvc_streaming(s) && !host_idle[s]; }

/* Which stream the sensor serves next: the only one that is wanted, or in turn (one second each) when both are. */
static int choose_stream(void) {
    static unsigned active;
    static uint32_t since;
    const unsigned w0 = wanted(0), w1 = wanted(1);
    if (w0 && w1) {
        if (apx_ticks() - since > TICKS_PER_SECOND) { active ^= 1u; since = apx_ticks(); }
        return (int)active;
    }
    if (!w0 && !w1) return -1;
    if (active != (w0 ? 0u : 1u)) { active = w0 ? 0u : 1u; since = apx_ticks(); }
    return (int)active;
}

static void tick_capture(void) {
    static unsigned selected = 0xffu;
    static uint32_t started;
    struct apx_camera_info info;
    if (nv[cap_i].state == NV_CAPTURING) {
        const uint32_t tp = apx_ticks();
        apx_camera_poll();
        apx_camera_get_info(&info);
        if (info.state == APX_CAMERA_DONE) stat.poll_done_ticks = apx_ticks() - tp;
        if (info.state == APX_CAMERA_ERROR) { ++stat.capture_errors; nv[cap_i].state = NV_FREE; return; }
        if (info.state != APX_CAMERA_DONE) return;
        stat.capture_ticks = apx_ticks() - started;
        /* Frame pacing: the sensor runs as fast as the exposure allows (up to 30 frames per second); the host asked for one frame per committed interval. Frames are accepted
         * at that rate: the due time advances by one interval for every frame taken, a frame is taken once it is due (never before, so the rate never exceeds the request, and
         * the sensor's frame period only shifts a frame later), and the others are dropped before the encoder sees them. */
        {
            static uint32_t due[2];
            const uint32_t now = apx_ticks();
            const unsigned s = nv[cap_i].stream;
            const uint32_t interval = (uint32_t)((uint64_t)apx_usb_uvc_frame_interval_us(s) * TICKS_PER_SECOND / 1000000u);
            if (!due[s] || (int32_t)(now - due[s]) > (int32_t)interval) due[s] = now; /* stream (re)started or fell behind by more than a frame: no catching up in a burst (a restart a little after the last frame would otherwise run at the sensor's rate until the lag is used up) */
            if ((int32_t)(now + TICKS_PER_SECOND / 1000u - due[s]) < 0) { ++stat.paced_out; nv[cap_i].state = NV_FREE; goto arm; } /* not due yet (1 ms of slack): drop, same buffer again */
            due[s] += interval;
        }
        ++stat.captured[nv[cap_i].stream];
        nv[cap_i].state = NV_FULL;
        cap_i ^= 1u;
        /* No return: arm the next frame at once. The sensor starts its next frame within about a millisecond of the end of this one, and a capture armed after
         * that start waits for the frame after it (two frame periods per frame, 80 ms, instead of one). */
    }
arm:
    if (nv[cap_i].state != NV_FREE) return; /* the encoder still holds this buffer */
    const int s = choose_stream();
    if (s < 0) return;
    if (selected != (unsigned)s) {
        const uint32_t t = apx_ticks();
        if (apx_camera_select(s == 0 ? APX_CAMERA_REAR : APX_CAMERA_FRONT)) { ++stat.capture_errors; return; }
        stat.select_ticks = apx_ticks() - t;
        selected = (unsigned)s;
        ++stat.switches;
    }
    nv[cap_i].stream = (unsigned)s;
    nv[cap_i].index = apx_usb_uvc_frame_index((unsigned)s);
    nv[cap_i].w = nv[cap_i].index == APX_UVC_FRAME_320x240 ? 320 : 640;
    nv[cap_i].h = nv[cap_i].index == APX_UVC_FRAME_320x240 ? 240 : 480;
    started = apx_ticks();
    /* The scaled path keeps the sensor running between frames (about 25 fps possible); a native capture_start restarts it each time and takes 233 ms. */
    if (apx_camera_scaled_start(nv[cap_i].buf, FRAME_CAPACITY, nv[cap_i].w, nv[cap_i].h, nv[cap_i].w)) { ++stat.capture_errors; return; }
    stat.start_call_ticks = apx_ticks() - started;
    nv[cap_i].state = NV_CAPTURING;
}

/* JPEG quality follows the frame budget. Frames go out straight from their buffer at about 10 MB/s (measured with examples/uvc-bench: 100 KB frames at 116 per second; the
 * payload copy TinyUSB would make capped it at 4.5 MB/s), so at 30 frames per second a frame may be 300 KB: more than the JPEG buffer holds, and the stock quality 90 (85 KB
 * for a busy 640x480 scene) fits at any rate. The budget is kept for slower links or a busier loop: a stream's quality is lowered when a frame is over its budget (the
 * interval the host committed times SEND_BYTES_PER_S, with 15% margin) and raised slowly while frames are well under it. */
#ifndef APX_UVC_SEND_BYTES_PER_S
#define APX_UVC_SEND_BYTES_PER_S 6000000u
#endif
#define JPEG_QUALITY_MIN 15u
#define JPEG_QUALITY_MAX 90u
static unsigned jpeg_quality[2] = { 75, 75 };
static void adapt_quality(unsigned s, uint32_t bytes) {
    uint32_t budget = (uint32_t)((uint64_t)APX_UVC_SEND_BYTES_PER_S * apx_usb_uvc_frame_interval_us(s) / 1000000u * 85u / 100u);
    if (budget > JPEG_CAPACITY - 4096u) budget = JPEG_CAPACITY - 4096u;
    unsigned q = jpeg_quality[s];
    if (bytes > budget) { const unsigned drop = bytes > budget + budget / 2 ? 8u : 3u; q = q > JPEG_QUALITY_MIN + drop ? q - drop : JPEG_QUALITY_MIN; }
    else if (bytes < budget * 70u / 100u && q < JPEG_QUALITY_MAX) ++q;
    jpeg_quality[s] = q;
}

static void tick_encode(void) {
    static uint32_t started;
    static int slot = -1;
    struct apx_jpeg_info info;
    if (nv[enc_i].state == NV_ENCODING) {
        apx_jpeg_poll();
        apx_jpeg_get_info(&info);
        if (info.state == APX_JPEG_ERROR) { ++stat.encode_errors; js[slot].state = J_FREE; nv[enc_i].state = NV_FREE; enc_i ^= 1u; return; }
        if (info.state != APX_JPEG_DONE) return;
        ++stat.encoded[nv[enc_i].stream];
        stat.last_bytes[nv[enc_i].stream] = info.bytes;
        adapt_quality(nv[enc_i].stream, info.bytes);
        stat.encode_ticks = apx_ticks() - started;
        js[slot].bytes = info.bytes;
        js[slot].state = J_READY;
        nv[enc_i].state = NV_FREE;
        enc_i ^= 1u;
        return;
    }
    if (nv[enc_i].state != NV_FULL) return;
    if (!wanted(nv[enc_i].stream)) { ++stat.dropped; nv[enc_i].state = NV_FREE; enc_i ^= 1u; return; } /* the client left meanwhile */
    slot = js[0].state == J_FREE ? 0 : js[1].state == J_FREE ? 1 : -1;
    if (slot < 0) return; /* both JPEG buffers are waiting to go out */
    const unsigned w = nv[enc_i].w, h = nv[enc_i].h;
    started = apx_ticks();
    apx_jpeg_set_encode_quality(jpeg_quality[nv[enc_i].stream]);
    const int enc_e = apx_jpeg_encode_start(nv[enc_i].buf, nv[enc_i].buf + w * h, w, h, w, w, h, 8, js[slot].buf, JPEG_CAPACITY - JPEG_LEAD);
    stat.encode_call_ticks = apx_ticks() - started;
    if (enc_e) {
        ++stat.encode_errors;
        nv[enc_i].state = NV_FREE;
        enc_i ^= 1u;
        return;
    }
    js[slot].state = J_ENCODING;
    js[slot].stream = nv[enc_i].stream;
    js[slot].seq = seq_next++;
    nv[enc_i].state = NV_ENCODING;
}

static void tick_send(void) {
    for (unsigned i = 0; i < 2; ++i)
        if (js[i].state == J_SENDING && !apx_usb_uvc_busy(js[i].stream)) { /* TinyUSB is done with the buffer */
            stat.send_ticks += apx_ticks() - js[i].sent_at; stat.send_bytes += js[i].bytes; ++stat.send_frames;
            js[i].state = J_FREE;
        }
    /* A finished frame of a stream nobody reads would keep a JPEG buffer (and so, in the end, the whole pipeline) waiting for ever: drop it. A frame already
     * queued on such a stream stays with TinyUSB until the client returns, and its buffer stays reserved; the other buffer keeps the other stream going. */
    for (unsigned i = 0; i < 2; ++i)
        if (js[i].state == J_READY && !wanted(js[i].stream)) { ++stat.dropped; js[i].state = J_FREE; }
    /* the oldest finished frame whose stream can take it (frames of one stream must go out in order) */
    int best = -1;
    for (unsigned i = 0; i < 2; ++i)
        if (js[i].state == J_READY && (best < 0 || (int32_t)(js[i].seq - js[best].seq) < 0)) best = (int)i;
    if (best < 0) return;
    if (!apx_usb_uvc_streaming(js[best].stream)) { ++stat.dropped; js[best].state = J_FREE; return; }
    if (apx_usb_uvc_busy(js[best].stream)) return;
    /* The two video endpoints share their packet RAM buffers: never start a frame on one while the other is still sending (a stream nobody reads is ignored, its stale packet is flushed). */
    if (apx_usb_uvc_busy(js[best].stream ^ 1u) && !host_idle[js[best].stream ^ 1u]) return;
    if (apx_usb_uvc_send_frame(js[best].stream, js[best].buf, js[best].bytes) == 0) {
        ++stat.queued[js[best].stream];
        js[best].sent_at = apx_ticks();
        js[best].state = J_SENDING;
    }
}

#if APX_USB_UAC
#ifndef APX_UVC_TONE
#define APX_UVC_TONE 0 /* 1 = the microphone endpoint carries a 1 kHz test tone instead of the microphone (for the host tests) */
#endif
#if APX_UVC_TONE
static const int16_t tone[32] = { 0, 1561, 3061, 4445, 5657, 6652, 7391, 7846, 8000, 7846, 7391, 6652, 5657, 4445, 3061, 1561, 0, -1561, -3061, -4445, -5657, -6652, -7391, -7846, -8000, -7846, -7391, -6652, -5657, -4445, -3061, -1561 };
/* A 1 kHz sine, produced at the sample rate from the 32768 Hz tick (the two clocks drift against the USB frame clock; the driver's ring absorbs it). */
static void tick_audio(void) {
    static uint32_t last, owed, phase;
    const uint32_t now = apx_ticks();
    if (!apx_usb_uac_streaming()) { last = now; owed = 0; return; }
    owed += (now - last) * APX_UAC_RATE;
    last = now;
    unsigned n = owed / TICKS_PER_SECOND;
    owed -= n * TICKS_PER_SECOND;
    if (n > 256) n = 256;
    int16_t block[256];
    for (unsigned i = 0; i < n; ++i) block[i] = tone[phase++ & 31u];
    if (n) apx_usb_uac_write_raw(block, n);
}
#else
/* The microphone: the SoC's recording DMA fills one buffer and stops (it does not wrap and does not report its position), but it writes the samples as they are
 * taken, 32 per millisecond. So the buffer is filled with a sentinel (-32768) first and the samples that have replaced it are forwarded to the USB ring as they appear.
 * Two buffers alternate. While one is being recorded the other is made ready (sentinel written and pushed out of the cache, a little per call), so that when the recording ends
 * the next one starts with a few register writes: the sample FIFO of the core only bridges a very short gap, and doing the preparation at that moment (3.3 ms) lost about 100
 * samples every second. The service routine is also called by the USB poll, so the handover is not left waiting for the main loop. */
#define MIC_CHUNK 32768u /* the largest buffer the DMA takes (64 KiB): a restart, and its 16 ms smoothed spot, once per 1.024 s */
#define MIC_PREP_STEP 2048u
static int16_t mic_buf[2][MIC_CHUNK] __attribute__((section(".jpeg_buffer"), aligned(32)));
static unsigned mic_which, mic_pos, mic_running, mic_started, mic_prep; /* mic_prep: samples of the idle buffer that are ready (MIC_CHUNK = all) */
static uint32_t mic_restarts, mic_start_errors;
static int mic_start(void) {
    int16_t *b = mic_buf[mic_which];
    for (unsigned i = 0; i < MIC_CHUNK; ++i) b[i] = -32768;
    mic_pos = 0;
    mic_prep = 0;
    return mic_started ? apx_mic_rearm(b, sizeof mic_buf[0]) : apx_mic_capture_start_continuous(b, sizeof mic_buf[0], APX_UAC_RATE);
}
static void mic_prepare(void) { /* one step of getting the idle buffer ready */
    if (mic_prep >= MIC_CHUNK) return;
    int16_t *b = mic_buf[mic_which ^ 1u] + mic_prep;
    for (unsigned i = 0; i < MIC_PREP_STEP; ++i) b[i] = -32768;
    apx_dma_clean(b, MIC_PREP_STEP * 2);
    apx_dma_invalidate(b, MIC_PREP_STEP * 2);
    mic_prep += MIC_PREP_STEP;
}
/* Every capture (re)start puts a decaying DC step at the start of the signal (about -1100 counts relaxing over ~500 samples, 15 ms: the microphone path's DC remover starts from
 * zero; it also happens with the analog stage left running). The first MIC_SKIP samples of a buffer are therefore not forwarded; the gap is bridged with a straight line from the
 * last sample sent to the first good one, which keeps the sample count (and so the timing) and leaves a 16 ms smoothed spot once per buffer. */
#define MIC_SKIP 512u
static int16_t mic_last;
static uint32_t mic_prev_start, mic_lat_sum, mic_lat_max, mic_dma_sum;
static uint32_t mic_total, mic_t0; /* samples forwarded since the stream started, and the tick it started at */
static void mic_forward(int finished) {
    int16_t *b = mic_buf[mic_which];
    int16_t out[256 + MIC_SKIP];
    unsigned n = 0;
    while (mic_pos < MIC_CHUNK) {
        const unsigned from = mic_pos & ~7u, limit = mic_pos + 256 < MIC_CHUNK ? mic_pos + 256 : MIC_CHUNK;
        apx_dma_invalidate(b + from, ((limit + 7u) / 8u * 8u - from) * 2u); /* whole 16-byte cache lines (8 samples) or the call refuses and the old sentinel stays in the cache */
        unsigned i = mic_pos;
        n = 0;
        while (i < limit && (finished || b[i] != -32768)) {
            const int16_t sample = b[i]; /* the level stage (DC blocker, mute, volume, AGC) is in the USB audio driver, controlled from the host's mixer */
            if (i == MIC_SKIP) { /* the first good sample: bridge the skipped ones */
                for (unsigned k = 0; k < MIC_SKIP; ++k) out[n++] = (int16_t)(mic_last + ((int)sample - mic_last) * (int)(k + 1) / (int)(MIC_SKIP + 1));
            }
            if (i >= MIC_SKIP) { out[n++] = sample; mic_last = sample; }
            ++i;
        }
        if (n) { apx_usb_uac_write(out, n); mic_total += n; n = 0; }
        mic_pos = i;
        if (i < limit) break; /* the DMA has not got further yet */
    }
}
static void tick_audio(void) {
    static unsigned busy; /* the routine is called from the USB poll, which the mic calls below can reach through a delay */
    if (busy) return;
    busy = 1;
    if (!apx_usb_uac_streaming()) {
        if (mic_running) {
            const uint32_t dt = apx_ticks() - mic_t0;
            apx_mic_stop(); mic_running = 0;
            printf("mic stream ended: %lu samples in %lu ticks = %lu Hz\n", (unsigned long)mic_total, (unsigned long)dt, (unsigned long)((uint64_t)mic_total * TICKS_PER_SECOND / (dt ? dt : 1)));
        }
        busy = 0;
        return;
    }
    struct apx_mic_info mi;
    if (!mic_running) {
        mic_started = 0;
        if (mic_start() == 0) { mic_running = 1; mic_started = 1; mic_total = 0; mic_t0 = apx_ticks(); mic_prev_start = 0; mic_lat_sum = mic_lat_max = mic_dma_sum = 0; } else ++mic_start_errors;
        busy = 0;
        return;
    }
    apx_mic_get_info(&mi);
    if (mi.state == APX_MIC_CAPTURING) {
        mic_forward(0);
        mic_prepare();
    } else if (mi.state == APX_MIC_DONE && mic_prep >= MIC_CHUNK) {
        /* the buffer is full and the next one is ready: start it at once, then take what is left of this one */
        const unsigned next = mic_which ^ 1u;
        if (apx_mic_rearm_prepared(mic_buf[next], sizeof mic_buf[0]) == 0) {
            const uint32_t t = apx_ticks();
            if (mic_prev_start) { const uint32_t lat = t - mic_prev_start - mi.ticks; mic_lat_sum += lat; if (lat > mic_lat_max) mic_lat_max = lat; mic_dma_sum += mi.ticks; }
            mic_prev_start = t;
            mic_forward(1);
            mic_which = next; mic_pos = 0; mic_prep = 0;
            ++mic_restarts;
        } else { mic_running = 0; ++mic_start_errors; }
    } else {
        /* the capture failed, or the next buffer was not ready in time: the slow way */
        if (mi.state == APX_MIC_DONE) mic_forward(1);
        mic_which ^= 1u;
        if (mic_start() == 0) { ++mic_restarts; mic_prev_start = apx_ticks(); } else { mic_running = 0; ++mic_start_errors; }
    }
    busy = 0;
}
#endif
#endif

static unsigned long ms(uint32_t ticks) { return (unsigned long)((uint64_t)ticks * 1000u / TICKS_PER_SECOND); }

int main(void) {
    printf("uvc-camera starting\n");
    apx_usb_init();
    int e = apx_camera_init();
    printf("camera init %d, %u sensors\n", e, apx_camera_count());
    uint32_t until = apx_ticks() + TICKS_PER_SECOND * APX_UVC_DEBUG_SECONDS;
    while (!apx_deadline_expired(until)) apx_debug_poll();
#if APX_UVC_SD
    {
        int sd = apx_usb_msc_use_sd();
        printf("mass storage: %s\n", sd == 0 ? "SD card, read-only" : "RAM disk (no usable SD card)");
        if (sd != 0) apx_usb_msc_set_disk(NULL);
    }
#endif
#if APX_USB_UAC && !APX_UVC_TONE
    printf("mic init %d\n", apx_mic_init()); /* now: it takes tens of milliseconds, which the USB link must not see while a host is recording */
#endif
#if APX_USB_UAC
    apx_usb_uac_set_service(tick_audio); /* run on every USB poll: also while the main loop waits inside a camera or delay call */
#endif
    if (apx_usb_set_mode(APX_USB_MODE_CAMERA) != 0) {
        printf("camera mode unavailable in this build\n");
        for (;;) apx_debug_poll();
    }
    printf("camera mode\n");
    uint32_t end = apx_ticks() + TICKS_PER_SECOND * APX_UVC_SECONDS, report = apx_ticks() + TICKS_PER_SECOND * 5;
    uint32_t sent_before[2] = { 0, 0 }, t_before = apx_ticks(), prev_iter = apx_ticks();
    uint32_t gaps[5] = { 0, 0, 0, 0, 0 }, longest = 0; /* main-loop iteration time: <1 ms, 1-2, 2-4, 4-8, >8 ms */
    while (APX_UVC_SECONDS == 0 || !apx_deadline_expired(end)) {
        {
            const uint32_t now = apx_ticks(), dt = now - prev_iter; /* ticks of 30.5 us */
            prev_iter = now;
            ++gaps[dt < 33 ? 0 : dt < 66 ? 1 : dt < 131 ? 2 : dt < 262 ? 3 : 4];
            if (dt > longest) longest = dt;
        }
        apx_usb_poll();
        (void)apx_camera_service(); /* writes camera control changes the host made (brightness, exposure, ...) to the sensor */
        tick_capture();
        watch_host();
        tick_send();
        tick_encode();
        if (apx_deadline_expired(report)) {
            report += TICKS_PER_SECOND * 5;
            const uint32_t now = apx_ticks();
            unsigned long fps10[2];
            for (unsigned s = 0; s < 2; ++s) {
                fps10[s] = (unsigned long)(apx_usb_uvc_frames_sent(s) - sent_before[s]) * 10u * TICKS_PER_SECOND / (now - t_before);
                sent_before[s] = apx_usb_uvc_frames_sent(s);
            }
            t_before = now;
#if APX_USB_UAC
            {
                struct apx_usb_uac_stats us;
                apx_usb_uac_get_stats(&us);
                #if !APX_UVC_TONE
                printf("mic: restarts %lu errors %lu samples %lu in %lu ticks, dma %lu ticks/buf, handover latency avg %lu max %lu ticks\n", (unsigned long)mic_restarts, (unsigned long)mic_start_errors, (unsigned long)mic_total, (unsigned long)(apx_ticks() - mic_t0), (unsigned long)(mic_restarts > 1 ? mic_dma_sum / (mic_restarts - 1) : 0), (unsigned long)(mic_restarts > 1 ? mic_lat_sum / (mic_restarts - 1) : 0), (unsigned long)mic_lat_max);
#endif
                printf("uac: stream %d packets %lu under %lu over %lu short %lu long %lu level %lu alt on/off %lu/%lu refused %lu gain x%lu/256 clamped %lu agc %lu\n", apx_usb_uac_streaming(), (unsigned long)us.packets, (unsigned long)us.underruns,
                       (unsigned long)us.overruns, (unsigned long)us.short_packets, (unsigned long)us.long_packets, (unsigned long)us.level, (unsigned long)us.alt_on, (unsigned long)us.alt_off, (unsigned long)us.send_refused, (unsigned long)us.gain_q8, (unsigned long)us.clipped, (unsigned long)us.agc);
            }
#endif
            {
                struct apx_camera_info ci;
                apx_camera_get_info(&ci);
                unsigned v[10] = { 0 }, e = 0;
                static const unsigned regs[10][2] = { {0,3}, {0,4}, {0,5}, {0,6}, {1,2}, {1,4}, {1,0x12}, {1,0x18}, {1,0x32}, {0,0x31} };
                for (unsigned i = 0; i < 10 && !e; ++i) e = (unsigned)apx_camera_sensor_read(regs[i][0], regs[i][1], &v[i]);
                printf("sensor %lu (err %u): exp %02x%02x vblank %02x%02x | cap %02x gain %02x contrast %02x bright %02x sw %02x mirror %02x\n", (unsigned long)ci.selected, e, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9]);
            }
            if (stat.send_frames)
                printf("send: JPEG quality %u/%u, %lu frames, avg %lu B, %lu us each = %lu KB/s\n", jpeg_quality[0], jpeg_quality[1], (unsigned long)stat.send_frames, (unsigned long)(stat.send_bytes / stat.send_frames),
                       (unsigned long)((uint64_t)stat.send_ticks * 1000000u / TICKS_PER_SECOND / stat.send_frames), (unsigned long)((uint64_t)stat.send_bytes * TICKS_PER_SECOND / 1024u / (stat.send_ticks ? stat.send_ticks : 1)));
#ifdef APX_DCD_DIAG
            { extern void apx_dcd_diag(void); extern void apx_uvc_diag(void); apx_dcd_diag(); apx_uvc_diag(); }
#endif
            printf("loop: <1ms %lu 1-2 %lu 2-4 %lu 4-8 %lu >8 %lu, longest %lu ms\n", (unsigned long)gaps[0], (unsigned long)gaps[1], (unsigned long)gaps[2], (unsigned long)gaps[3], (unsigned long)gaps[4], ms(longest));
            printf("uvc: s0 %d/%lu.%lu fps idle %u frame#%u | s1 %d/%lu.%lu fps idle %u frame#%u | cap %lu/%lu enc %lu/%lu q %lu/%lu err %lu/%lu drop %lu pace %lu sw %lu (%lu ms) | cap %lu ms enc %lu ms [start %lu ms, poll-done %lu ms, enc-start %lu ms] | last %lu/%lu B | msc rd %lu\n",
                   apx_usb_uvc_streaming(0), fps10[0] / 10, fps10[0] % 10, host_idle[0], apx_usb_uvc_frame_index(0), apx_usb_uvc_streaming(1), fps10[1] / 10, fps10[1] % 10,
                   host_idle[1], apx_usb_uvc_frame_index(1), (unsigned long)stat.captured[0], (unsigned long)stat.captured[1], (unsigned long)stat.encoded[0],
                   (unsigned long)stat.encoded[1], (unsigned long)stat.queued[0], (unsigned long)stat.queued[1], (unsigned long)stat.capture_errors,
                   (unsigned long)stat.encode_errors, (unsigned long)stat.dropped, (unsigned long)stat.paced_out, (unsigned long)stat.switches, ms(stat.select_ticks), ms(stat.capture_ticks),
                   ms(stat.encode_ticks), ms(stat.start_call_ticks), ms(stat.poll_done_ticks), ms(stat.encode_call_ticks), (unsigned long)stat.last_bytes[0], (unsigned long)stat.last_bytes[1], (unsigned long)apx_usb_msc_blocks_read());
        }
    }
    printf("uvc-camera: back to debug mode\n");
    apx_usb_set_mode(APX_USB_MODE_DEBUG);
    for (;;) apx_debug_poll();
}
