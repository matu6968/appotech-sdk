#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <stdio.h>
int main(void) {
    printf("JT5295 SDK starting; picolibc; inherited 96 MHz estimate\n");
    apx_usb_init();
    for(;;) apx_debug_poll();
}
