#include <appotech/debug.h>
#include <appotech/lvgl_port.h>
#include <appotech/usb.h>
#include <appotech/spi.h>
#include <appotech/trace.h>
#include <appotech/uart.h>

#include <demos/lv_demos.h>
#include <stdio.h>

#if APX_LVGL_PERF
/* Show the summary on screen as LVGL does by default, and also print every row as CSV (the table is taller than the
 * 240-pixel panel, so the last scenes are cut off on screen). Read it with `apxtool debug logs` or the UART. */
static void benchmark_report(const lv_demo_benchmark_summary_t *summary)
{
    lv_demo_benchmark_summary_display(summary);
    lv_display_t *display = lv_display_get_default();
    printf("LVGL benchmark results (%dx%d)\r\n", (int)lv_display_get_horizontal_resolution(display),
           (int)lv_display_get_vertical_resolution(display));
    printf("Name, Avg. CPU %%, Avg. FPS, Avg. ms, render ms, flush ms\r\n");
    for(const lv_demo_benchmark_scene_dsc_t *scene = summary->scenes; scene->create_cb; ++scene) {
        if(!scene->measurement_cnt) { printf("%s, N/A\r\n", scene->name); continue; }
        unsigned count = scene->measurement_cnt;
        unsigned render = scene->render_avg_time / count, flush = scene->flush_avg_time / count;
        printf("%s, %u, %u, %u, %u, %u\r\n", scene->name, (unsigned)(scene->cpu_avg_usage / count),
               (unsigned)(scene->fps_avg / count), render + flush, render, flush);
    }
    if(summary->valid_scene_cnt) {
        unsigned n = (unsigned)summary->valid_scene_cnt;
        unsigned render = (unsigned)summary->total_avg_render_time / n, flush = (unsigned)summary->total_avg_flush_time / n;
        printf("All scenes avg., %u, %u, %u, %u, %u\r\n", (unsigned)summary->total_avg_cpu / n,
               (unsigned)summary->total_avg_fps / n, render + flush, render, flush);
    }
    printf("LVGL benchmark done\r\n");
}
#endif

int main(void)
{
    static char demo_name[] = APX_LVGL_DEMO;
    char *demo_args[] = {demo_name};

    apx_trace(APX_TRACE_LVGL,0);apx_mark(APX_TRACE_LVGL,0);
    apx_usb_init();
    apx_trace(APX_TRACE_LVGL+1,0);apx_mark(APX_TRACE_LVGL+1,0);
    /* printf/stderr also go to UART0 TX (PA7, 115200 8N1) so a stuck or rebooting camera leaves a trail. */
    if(!apx_uart_init(115200, 6, -1)) apx_uart_console_enable(1);
    /* The renderer's code and assets live above the range the BootROM loads: copy them in first. */
    int overlay = apx_spi_overlay_load();
    apx_trace(APX_TRACE_LVGL+2,(uint32_t)overlay);apx_mark(APX_TRACE_LVGL+2,(uint32_t)overlay);
    if(overlay) {
        printf("SPI overlay load failed: %d\n", overlay);
        for(;;) apx_debug_poll();
    }
    int port = apx_lvgl_port_init();
    apx_trace(APX_TRACE_LVGL+3,(uint32_t)port);apx_mark(APX_TRACE_LVGL+3,(uint32_t)port);
    if(port < 0) {
        printf("LVGL port initialization failed\n");
        for(;;) apx_debug_poll();
    }

#if APX_LVGL_PERF
    lv_demo_benchmark_set_end_cb(benchmark_report);
#endif
    int created = lv_demos_create(demo_args, 1);
    apx_trace(APX_TRACE_LVGL+4,(uint32_t)created);apx_mark(APX_TRACE_LVGL+4,(uint32_t)created);
    if(!created) {
        lv_demos_show_help();
        for(;;) apx_lvgl_port_poll();
    }

    for(unsigned first = 1;;) {
        int polled = apx_lvgl_port_poll();
        if(first) { apx_trace(APX_TRACE_LVGL+5,(uint32_t)polled);apx_mark(APX_TRACE_LVGL+5,(uint32_t)polled);first = 0; }
        if(polled < 0) {
            printf("LVGL display/input service failed\n");
            for(;;) apx_debug_poll();
        }
    }
}
