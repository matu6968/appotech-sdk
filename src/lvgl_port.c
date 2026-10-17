#include <appotech/buttons.h>
#include <appotech/debug.h>
#include <appotech/display.h>
#include <appotech/lvgl_port.h>
#include <appotech/soc.h>
#include <appotech/watchdog.h>

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Post-mortem record for a crash that leaves no log (watchdog reset): live statistics are kept in on-chip SRAM,
 * which a reset does not clear, and the previous run's record is copied aside at start-up. Read it from the
 * running SDK with `debug read 0x3000 128` (live, previous run at +0x40) or, after a failed boot, from the loader.
 *   +0 magic 'PMRT'  +4 run counter  +8 smallest stack headroom (bytes)  +12 smallest LVGL heap free
 *   +16 smallest biggest-free-block  +20 longest lv_timer_handler (32 kHz ticks)  +24 handler start tick, 0 = not inside
 *   +28 last tick sampled  +32 last key  +36 handler calls  +40 LVGL assert line  +44.. assert file tail (20 bytes) */
#define PM_BASE ((volatile uint32_t *)0x3000u)
#define PM_MAGIC 0x54524d50u
extern unsigned char __stack_bottom[],__stack_top[];
static unsigned stack_headroom(void)
{
    const uint32_t *word = (const uint32_t *)__stack_bottom;
    unsigned free_bytes = 0;
    while((const unsigned char *)word < __stack_top && *word == 0xa5a5a5a5u) { free_bytes += 4; ++word; }
    return free_bytes;
}
static uint32_t pm_next_sample;
#if APX_LVGL_INSTRUMENT
/* Entered-function trail for a crash with no other evidence: LVGL is built with -finstrument-functions and every
 * call records (function, call site) in a 30-entry ring at SRAM 0x3200 (word 0 = running count). The next boot
 * copies it to 0x3300 before LVGL starts. Map addresses with `or1k-appotech-elf-nm -n` on the ELF. */
#define TRAIL ((volatile uint32_t *)0x3200u)
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site)
{
    volatile uint32_t *r = TRAIL;
    uint32_t i = r[0];
    unsigned slot = 2 + 2 * (i % 30);
    r[slot] = (uint32_t)(uintptr_t)fn; r[slot + 1] = (uint32_t)(uintptr_t)site;
    r[0] = i + 1;
    /* Instrumented LVGL is slow enough to outlast the watchdog on its own. Keep feeding it for up to 8 s
     * of one lv_timer_handler call, so a genuine endless loop still ends in a reset with its trail intact. */
    if(!(i & 0x7ff)) {
        uint32_t started = ((volatile uint32_t *)0x3000u)[6];
        if(!started || (uint32_t)(apx_ticks() - started) < 8u * 32768u) apx_watchdog_feed();
    }
}
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site) { (void)fn; (void)site; }
#endif
static void postmortem_start(void)
{
#if APX_LVGL_INSTRUMENT
    for(unsigned i = 0; i < 64; ++i) ((volatile uint32_t *)0x3300u)[i] = TRAIL[i];
    TRAIL[0] = 0;
#endif
    volatile uint32_t *live = PM_BASE;
    if(live[0] == PM_MAGIC) { for(unsigned i = 0; i < 16; ++i) live[16 + i] = live[i]; }
    else for(unsigned i = 0; i < 16; ++i) live[16 + i] = 0;
    uint32_t runs = (live[0] == PM_MAGIC ? live[1] : 0) + 1;
    for(unsigned i = 0; i < 16; ++i) live[i] = 0;
    live[0] = PM_MAGIC; live[1] = runs; live[2] = 0xffffffffu; live[3] = 0xffffffffu; live[4] = 0xffffffffu;
}
static void postmortem_sample(void)
{
    volatile uint32_t *live = PM_BASE;
    unsigned stack = stack_headroom();
    if(stack < live[2]) live[2] = stack;
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    if(monitor.free_size < live[3]) live[3] = monitor.free_size;
    if(monitor.free_biggest_size < live[4]) live[4] = monitor.free_biggest_size;
    live[7] = apx_ticks();
}
_Noreturn void apx_lvgl_assert(const char *file, int line)
{
    volatile uint32_t *live = PM_BASE;
    live[10] = (uint32_t)line;
    size_t n = strlen(file), skip = n > 20 ? n - 20 : 0;
    volatile char *out = (volatile char *)&live[11];
    for(unsigned i = 0; i < 20; ++i) out[i] = file[skip + i] ? file[skip + i] : 0;
    postmortem_sample();
    printf("LVGL assert at %s:%d (stack headroom %u)\n", file, line, stack_headroom());
    for(;;) apx_debug_poll(); /* stay alive and answer the host instead of waiting for the watchdog */
}

static uint16_t framebuffer[APX_DISPLAY_WIDTH * APX_DISPLAY_HEIGHT]
    __attribute__((section(".large_buffer"), aligned(32)));
static lv_display_t *display;
static lv_indev_t *keypad;
static lv_group_t *group;
static uint32_t tick_previous, tick_fraction;
static uint32_t held_key;
static unsigned flush_pending, port_error;

static uint32_t key_from_buttons(uint32_t state)
{
    if(state & APX_BUTTON_UP) return LV_KEY_PREV;
    if(state & APX_BUTTON_DOWN) return LV_KEY_NEXT;
    if(state & APX_BUTTON_LEFT) return LV_KEY_LEFT;
    if(state & APX_BUTTON_RIGHT) return LV_KEY_RIGHT;
    if(state & APX_BUTTON_SHUTTER) return LV_KEY_ENTER;
    if(state & APX_BUTTON_POWER) return LV_KEY_ESC;
    return 0;
}

static void keypad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    struct apx_buttons_info info;
    apx_buttons_get_info(&info);
    uint32_t key = key_from_buttons(info.state);

    if(held_key && key != held_key) {
        data->key = held_key;
        data->state = LV_INDEV_STATE_RELEASED;
        held_key = 0;
        return;
    }

    if(!held_key) held_key = key;
    data->key = held_key;
    data->state = held_key ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void display_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if(flush_pending || apx_display_status() != APX_DISPLAY_READY) {
        port_error = 1;
        lv_display_flush_ready(disp);
        return;
    }

    unsigned width = (unsigned)(area->x2 - area->x1 + 1);
    unsigned height = (unsigned)(area->y2 - area->y1 + 1);
    int result;
    if(area->x1 == 0 && area->y1 == 0 &&
       width == APX_DISPLAY_WIDTH && height == APX_DISPLAY_HEIGHT) {
        result = apx_display_blit_dma(0, 0, width, height,
                                      (const uint16_t *)px_map);
    } else {
        const uint16_t *pixels = (const uint16_t *)px_map +
            (unsigned)area->y1 * APX_DISPLAY_WIDTH + (unsigned)area->x1;
        result = apx_display_blit((unsigned)area->x1, (unsigned)area->y1,
                                  width, height, pixels, APX_DISPLAY_WIDTH);
    }

    if(result < 0) {
        port_error = 1;
        lv_display_flush_ready(disp);
        return;
    }
    flush_pending = 1;
}

/* LVGL's single-buffer refresh waits for the previous flush with `while(disp->flushing);` unless a wait callback
 * exists. The SDK only completed a flush from apx_lvgl_port_poll(), outside lv_timer_handler(), so a frame that
 * started while the display DMA was still running spun forever and the watchdog reset the camera (found with the
 * function trail: last calls refr_configured_layer -> wait_for_flushing). Service the display here instead. */
static void display_flush_wait(lv_display_t *disp)
{
    (void)disp;
    uint32_t deadline = apx_ticks() + 2u * 32768u;
    while(flush_pending) {
        apx_debug_poll();
        apx_buttons_poll();
        apx_display_poll();
        apx_maintenance();
        unsigned status = apx_display_status();
        if(status == APX_DISPLAY_READY) { flush_pending = 0; break; }
        if(status == APX_DISPLAY_ERROR || apx_deadline_expired(deadline)) { port_error = 1; flush_pending = 0; break; }
    }
}

int apx_lvgl_port_init(void)
{
    if(display || keypad || group) return -1;

    apx_buttons_init();
    apx_display_init();
    while(apx_display_status() == APX_DISPLAY_INITIALIZING) {
        apx_debug_poll();
        apx_buttons_poll();
        apx_display_poll();
        apx_maintenance();
    }
    if(apx_display_status() != APX_DISPLAY_READY) return -1;

    postmortem_start();
    lv_init();
    display = lv_display_create(APX_DISPLAY_WIDTH, APX_DISPLAY_HEIGHT);
    if(!display) return -1;
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, NULL, sizeof(framebuffer),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, display_flush);
    lv_display_set_flush_wait_cb(display, display_flush_wait);

    group = lv_group_create();
    if(!group) return -1;
    lv_group_set_default(group);

    keypad = lv_indev_create();
    if(!keypad) return -1;
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad, keypad_read);
    lv_indev_set_display(keypad, display);
    lv_indev_set_group(keypad, group);

    tick_previous = apx_ticks();
    tick_fraction = 0;
    held_key = 0;
    flush_pending = port_error = 0;
    return 0;
}

int apx_lvgl_port_poll(void)
{
    if(!display || !keypad || !group) return -1;

    apx_debug_poll();
    apx_buttons_poll();
    apx_display_poll();
    apx_maintenance();
    if(apx_display_status() == APX_DISPLAY_ERROR) port_error = 1;
    if(port_error) return -1;

    if(flush_pending && apx_display_status() == APX_DISPLAY_READY) {
        flush_pending = 0;
        lv_display_flush_ready(display);
    }

    uint32_t now = apx_ticks();
    uint32_t elapsed = (now - tick_previous) * 1000u + tick_fraction;
    tick_previous = now;
    lv_tick_inc(elapsed / 32768u);
    tick_fraction = elapsed % 32768u;
    {
        volatile uint32_t *live = PM_BASE;
        if(held_key) live[8] = held_key;
        uint32_t started = apx_ticks();
        live[6] = started ? started : 1;
        lv_timer_handler();
        uint32_t spent = apx_ticks() - started;
        live[6] = 0;
        if(spent > live[5]) live[5] = spent;
        ++live[9];
        if((int32_t)(now - pm_next_sample) >= 0) { postmortem_sample(); pm_next_sample = now + 3277; }
    }
    return 0;
}

lv_group_t *apx_lvgl_port_group(void)
{
    return group;
}
