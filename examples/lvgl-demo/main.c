#include <appotech/debug.h>
#include <appotech/lvgl_port.h>
#include <appotech/usb.h>
#include <appotech/spi.h>
#include <appotech/trace.h>
#include <appotech/uart.h>

#include <demos/lv_demos.h>
#include <stdio.h>

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
