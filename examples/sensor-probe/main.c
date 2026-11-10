/* Sensor register and frame-rate probe for the Silvergear's ID 0x39 sensors. Streams 320x240 frames continuously through the scaled capture path, measures the interval
 * between completed frames, and answers APXD camera-service commands (operation 11) so the sensor can be examined and changed live from the host (tools/sensor_probe.py):
 *   0x20 read   in: u32 page, u32 reg            out: u32 value
 *   0x21 write  in: u32 page, u32 reg, u32 value
 *   0x22 dump   in: u32 page                     out: 256 x u16 (value, or 0xffff when the read failed)
 *   0x23 stats  out: u32 frames, last, average, min, max (ticks of 1/32768 s, over the frames since the last reset), errors
 *   0x24 reset the statistics
 *   0x25 select in: u32 camera (0 rear, 1 front)

 *   0x27 encode in: u32 selector (0..15, stock 8), optional u32 quality (0 = stock tables, 1..100)  out: u32 jpeg bytes, u32 encode ticks, u32 address of the JPEG in RAM (read it with the debug link): stops the stream, takes
 *                one clean frame, encodes it with the hardware JPEG engine, and goes back to streaming
 *   0x26 stats2 out: of the last frame, from every 8th luma pixel and every 4th chroma pair: u32 mean luma, min, max, standard deviation of luma, mean chroma distance from neutral
 *                (|U-128|+|V-128|), mean U, mean V
 * Nothing here writes to storage. */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/camera.h>
#include <appotech/jpeg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PROBE_W
#define PROBE_W 320u
#define PROBE_H 240u
#endif
#define W PROBE_W
#define H PROBE_H
static uint8_t frame[W * H * 3 / 2 + 64] __attribute__((section(".large_buffer"), aligned(32)));
static uint8_t jpeg_out[128 * 1024] __attribute__((section(".jpeg_buffer"), aligned(32)));
static uint32_t frames, last_ticks, sum_ticks, min_ticks = ~0u, max_ticks, count, errors, prev;
static unsigned selected, capturing;

static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void reset_stats(void) { sum_ticks = count = max_ticks = 0; min_ticks = ~0u; prev = 0; }

uint32_t apx_camera_command(unsigned command, const uint8_t *in, size_t len, uint8_t *out, size_t *size) {
    *size = 0;
    unsigned value = 0;
    int e = 0;
    switch (command) {
    case 0x20:
        if (len != 8) return 0x300 + APX_CAMERA_INVALID;
        e = apx_camera_sensor_read(get32(in), get32(in + 4), &value);
        put32(out, value); *size = 4;
        return e ? 0x300 + (uint32_t)e : 0;
    case 0x21:
        if (len != 12) return 0x300 + APX_CAMERA_INVALID;
        e = apx_camera_sensor_write(get32(in), get32(in + 4), get32(in + 8));
        return e ? 0x300 + (uint32_t)e : 0;
    case 0x22:
        if (len != 4) return 0x300 + APX_CAMERA_INVALID;
        for (unsigned r = 0; r < 256; ++r) {
            e = apx_camera_sensor_read(get32(in), r, &value);
            const unsigned v = e ? 0xffffu : value;
            out[2 * r] = (uint8_t)v; out[2 * r + 1] = (uint8_t)(v >> 8);
        }
        *size = 512;
        return 0;
    case 0x23:
        put32(out, frames); put32(out + 4, last_ticks); put32(out + 8, count ? sum_ticks / count : 0); put32(out + 12, count ? min_ticks : 0); put32(out + 16, max_ticks); put32(out + 20, errors);
        *size = 24;
        return 0;
    case 0x24:
        reset_stats();
        return 0;
    case 0x25:
        if (len != 4 || get32(in) > 1) return 0x300 + APX_CAMERA_INVALID;
        apx_camera_stop(); capturing = 0;
        e = apx_camera_select(get32(in) ? APX_CAMERA_FRONT : APX_CAMERA_REAR);
        selected = get32(in); reset_stats();
        return e ? 0x300 + (uint32_t)e : 0;
    case 0x26: {
        apx_dma_invalidate(frame, W * H);
        unsigned long sum = 0, sq = 0, n = 0; unsigned mn = 255, mx = 0;
        for (unsigned i = 0; i < W * H; i += 8) { const unsigned v = frame[i]; sum += v; sq += (unsigned long)v * v; ++n; if (v < mn) mn = v; if (v > mx) mx = v; }
        const unsigned long mean = sum / n, var = sq / n - mean * mean;
        unsigned sd = 0; while ((unsigned long)(sd + 1) * (sd + 1) <= var) ++sd;
        unsigned long cd = 0, su = 0, sv = 0, cn = 0;
        for (unsigned i = W * H; i + 1 < W * H * 3 / 2; i += 8) { /* NV21: V then U */
            const int v = frame[i], u = frame[i + 1];
            cd += (unsigned)((u > 128 ? u - 128 : 128 - u) + (v > 128 ? v - 128 : 128 - v)); su += (unsigned)u; sv += (unsigned)v; ++cn;
        }
        put32(out, (uint32_t)mean); put32(out + 4, mn); put32(out + 8, mx); put32(out + 12, sd); put32(out + 16, (uint32_t)(cd / cn)); put32(out + 20, (uint32_t)(su / cn)); put32(out + 24, (uint32_t)(sv / cn));
        *size = 28;
        return 0;
    }
    case 0x27: {
        if (len != 4 && len != 8) return 0x300 + APX_CAMERA_INVALID;
        apx_jpeg_set_encode_quality(len == 8 ? get32(in + 4) : 0);
        apx_camera_stop();
        struct apx_camera_info ci;
        if (apx_camera_scaled_start(frame, sizeof frame, W, H, W)) return 0x300 + APX_CAMERA_HARDWARE;
        do { apx_camera_poll(); apx_camera_get_info(&ci); } while (ci.state == APX_CAMERA_CAPTURING);
        if (ci.state != APX_CAMERA_DONE) return 0x300 + APX_CAMERA_HARDWARE;
        if (apx_jpeg_encode_start(frame, frame + W * H, W, H, W, W, H, get32(in), jpeg_out, sizeof jpeg_out)) return 0x400;
        struct apx_jpeg_info ji;
        do { apx_jpeg_poll(); apx_jpeg_get_info(&ji); } while (ji.state == APX_JPEG_ENCODING);
        capturing = 0; apx_camera_stop();
        if (ji.state != APX_JPEG_DONE) return 0x400 + ji.error;
        put32(out, ji.bytes); put32(out + 4, ji.ticks); put32(out + 8, (uint32_t)(uintptr_t)jpeg_out);
        *size = 12;
        return 0;
    }
    default:
        return 0x300 + APX_CAMERA_UNSUPPORTED;
    }
}

int main(void) {
    printf("sensor-probe starting\n");
    apx_usb_init();
    int e = apx_camera_init();
    printf("camera init %d, %u sensors\n", e, apx_camera_count());
    if (e == 0) { e = apx_camera_select(APX_CAMERA_REAR); printf("select rear %d\n", e); }
    for (;;) {
        apx_debug_poll();
        if (!capturing) {
            if (apx_camera_scaled_start(frame, sizeof frame, W, H, W) == 0) capturing = 1; else ++errors;
            continue;
        }
        apx_camera_poll();
        struct apx_camera_info info;
        apx_camera_get_info(&info);
        if (info.state == APX_CAMERA_ERROR) { ++errors; capturing = 0; continue; }
        if (info.state != APX_CAMERA_DONE) continue;
        const uint32_t now = apx_ticks();
        ++frames;
        if (prev) {
            last_ticks = now - prev; sum_ticks += last_ticks; ++count;
            if (last_ticks < min_ticks) min_ticks = last_ticks;
            if (last_ticks > max_ticks) max_ticks = last_ticks;
        }
        prev = now;
        capturing = 0; /* re-armed at the top of the loop, at once */
    }
}
