/* Cycle the USB device between modes: the APXD debug link, then mass storage (a RAM disk), then the debug link again.
 *
 *   APX_MODES_DEBUG_SECONDS   time in debug mode before each switch (default 8): enough to run `apxtool debug info`
 *   APX_MODES_MSC_SECONDS     time in mass-storage mode (default 120); the host cannot reach APXD then, so the example
 *                             switches back by itself
 *   APX_MODES_ROUNDS          number of debug->MSC->debug rounds, 0 = forever (default 0)
 *   APX_MODES_SD              1 = present the SD card to the host, READ-ONLY (falls back to the RAM disk if there is
 *                             no usable card); 2 = the SD card, WRITABLE by the host; 0 = the RAM disk (default). Build with EXAMPLE_DEFS=-DAPX_MODES_SD=1 (or 2).
 *
 * With the legacy USB stack, or APX_USB_MSC=0, the switch is refused and the example stays in debug mode. */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/usb_msc.h>
#include <appotech/debug.h>
#include <stdio.h>

#ifndef APX_MODES_DEBUG_SECONDS
#define APX_MODES_DEBUG_SECONDS 8
#endif
#ifndef APX_MODES_MSC_SECONDS
#define APX_MODES_MSC_SECONDS 120
#endif
#ifndef APX_MODES_SD
#define APX_MODES_SD 0
#endif
#ifndef APX_MODES_ROUNDS
#define APX_MODES_ROUNDS 0
#endif

static void run_debug(unsigned seconds) {
    uint32_t until = apx_ticks() + 32768u * seconds;
    while (!apx_deadline_expired(until)) apx_debug_poll();
}

static void run_msc(unsigned seconds) {
    uint32_t until = apx_ticks() + 32768u * seconds, report = apx_ticks() + 32768u * 5;
    while (!apx_deadline_expired(until)) {
        apx_usb_poll();
        if (apx_deadline_expired(report)) {
            report += 32768u * 5;
            printf("usb-modes: msc configured=%d speed=%s blocks read=%lu written=%lu\n", apx_usb_configured(),
                   apx_usb_high_speed() ? "high" : "full", (unsigned long)apx_usb_msc_blocks_read(),
                   (unsigned long)apx_usb_msc_blocks_written());
        }
    }
}

int main(void) {
    printf("usb-modes starting\n");
    apx_usb_init();
    unsigned rounds_left = APX_MODES_ROUNDS; /* 0 = forever */
    for (unsigned round = 0; APX_MODES_ROUNDS == 0 || rounds_left-- > 0; ++round) {
        run_debug(APX_MODES_DEBUG_SECONDS);
#if APX_MODES_SD
        {
            int sd = APX_MODES_SD == 2 ? apx_usb_msc_use_sd_writable() : apx_usb_msc_use_sd();
            if (sd != 0) {
                printf("usb-modes: SD card unavailable (%d), presenting the RAM disk\n", sd);
                apx_usb_msc_set_disk(NULL);
            } else {
                printf("usb-modes: presenting the SD card %s\n", APX_MODES_SD == 2 ? "WRITABLE" : "read-only");
            }
        }
#endif
        printf("usb-modes: round %u, switching to MSC\n", round);
        if (apx_usb_set_mode(APX_USB_MODE_MSC) != 0) {
            printf("usb-modes: MSC unavailable in this build, staying in debug mode\n");
            for (;;) apx_debug_poll();
        }
        run_msc(APX_MODES_MSC_SECONDS);
        printf("usb-modes: back to debug mode (msc: %lu blocks read, %lu written)\n",
               (unsigned long)apx_usb_msc_blocks_read(), (unsigned long)apx_usb_msc_blocks_written());
        apx_usb_set_mode(APX_USB_MODE_DEBUG);
    }
    for (;;) apx_debug_poll();
}
