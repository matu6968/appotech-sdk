/* Host model of the USB control-endpoint address handling in src/usb.c.
 * Regression test for a hang found on hardware: a SETUPEND between SET_ADDRESS and its status stage
 * cleared pending_address but not address_wait, so the next status-stage interrupt wrote 0xff into FADDR
 * and the device disappeared from the address the host had just assigned. */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t intrusb,intrtx,txcsr,rxcount;
static uint8_t setup_bytes[8];
static unsigned faddr_writes,last_faddr;
static int faddr_written;
uint8_t apx_test_fifo(unsigned i) { return setup_bytes[i]; }
uint32_t apx_ticks(void) { static uint32_t t;return t+=10; }
void apx_maintenance(void) {}
void apx_delay_ms(uint32_t ms) { (void)ms; }
void apx_debug_reset(void) {}
uint32_t apx_spr_read(uint32_t spr) {
    unsigned offset=(spr-0xa000)/4;
    if(spr<0xa000||spr>=0xa200) return 0;
    switch(offset) {
    case 0x0a: { uint32_t v=intrusb;intrusb=0;return v; }   /* INTRUSB, read-to-clear */
    case 0x02: { uint32_t v=intrtx&255;return v; }          /* INTRTX low byte (cleared below, as one 16-bit read) */
    case 0x03: { uint32_t v=(intrtx>>8)&255;intrtx=0;return v; }
    case 0x12: return txcsr&255;
    case 0x13: return 0;
    case 0x18: return rxcount&255;
    case 0x19: return 0;
    default: return 0;
    }
}
void apx_spr_write(uint32_t spr,uint32_t value) {
    if(spr==0xa000) { ++faddr_writes;last_faddr=value&255;faddr_written=1; }
    if(spr==0xa000+4*0x12) txcsr=value&255; /* the driver arms the next stage; tests set the status explicitly */
}
#define SETUP(t,r,v,i,l) do { uint8_t b[8]={t,r,v&255,v>>8,i&255,i>>8,l&255,l>>8};memcpy(setup_bytes,b,8);rxcount=8;txcsr=1; } while(0)
static void deliver_set_address(unsigned address) { SETUP(0x00,0x05,address,0,0);apx_usb_poll(); }

int main(void) {
    /* Normal: SET_ADDRESS, then the status stage completes (TX interrupt, no TXPKTRDY) -> FADDR = address. */
    deliver_set_address(1);
    assert(!faddr_written);                   /* not before the status stage */
    txcsr=0;intrtx=1;apx_usb_poll();
    assert(faddr_written&&last_faddr==1&&faddr_writes==1);
    txcsr=0;intrtx=1;apx_usb_poll();          /* a later, unrelated TX interrupt must not rewrite it */
    assert(faddr_writes==1);

    /* SETUPEND before the status stage completes: the address is cancelled, never 0xff. */
    faddr_written=0;faddr_writes=0;
    deliver_set_address(2);
    txcsr=0x10;intrtx=0;apx_usb_poll();       /* SETUPEND */
    txcsr=0;intrtx=1;apx_usb_poll();          /* the TX interrupt that used to apply pending_address == -1 */
    assert(!faddr_written);
    for(unsigned i=0;i<3;++i) { txcsr=0;intrtx=1;apx_usb_poll(); }
    assert(!faddr_written);

    /* The host retries: the next SET_ADDRESS works normally afterwards. */
    deliver_set_address(2);
    txcsr=0;intrtx=1;apx_usb_poll();
    assert(faddr_written&&last_faddr==2&&faddr_writes==1);

    /* SETUPEND in the very poll that also sees the TX interrupt. */
    faddr_written=0;faddr_writes=0;
    deliver_set_address(3);
    txcsr=0x10;intrtx=1;apx_usb_poll();
    assert(!faddr_written);

    puts("USB model: SET_ADDRESS applies once; SETUPEND cancels it instead of writing FADDR=0xff");
    return 0;
}
