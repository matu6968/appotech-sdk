#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/printer.h>
#include <appotech/camera.h>
#include <appotech/battery.h>
#include <appotech/power.h>
#include <string.h>
#include <stdio.h>

/* No camera or display driver is linked or initialized in this diagnostic. */
void apx_camera_get_info(struct apx_camera_info *out) {memset(out,0,sizeof(*out));}

int main(void) {
    apx_usb_init();
    int power=apx_power_domain_enable(),battery=apx_battery_init();
    printf("Printer test ready; power=%d battery=%d; waiting for host commands\n",power,battery);
    for(;;) {
        apx_debug_poll();
        if(!apx_usb_configured()||apx_usb_suspended())apx_printer_cancel();
        apx_battery_poll();apx_printer_poll();apx_maintenance();
    }
}
