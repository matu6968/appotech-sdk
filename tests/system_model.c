/* Actual watchdog/power/APXD implementations; no physical hardware access. */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/watchdog.h>
#include <appotech/power.h>
#include <appotech/clock.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <appotech/exception.h>
void apx_exception_expect_reset(void) {}
int apx_exception_previous(struct apx_exception *out) { (void)out;return 0; }
const char *apx_exception_name(uint32_t vector) { (void)vector;return ""; }
static uint32_t regs[65536],ticks,storage_status;
static unsigned watchdog_writes,pm_writes,pm_phase,pm_command,pm_busy,tx_idle=1,event,expect_event;
static unsigned order,ack_order,stop_order,display_order,sd_order,usb_order;
static uint8_t pm[16],incoming[512],outgoing[4096+24];
static size_t incoming_size,outgoing_size;
static jmp_buf terminal;
static unsigned operation=APXD_SYSTEM;
static unsigned camera_polls;
void apx_camera_poll(void) {++camera_polls;}
extern void apx_debug_reset(void);
unsigned char __display_dma_start[1],__display_dma_end[1],__extra_ram_start[1],__extra_ram_end[1],__jpeg_buffer_start[1],__jpeg_buffer_end[1];
uint32_t apx_ticks(void) { return ticks++; }
void apx_delay_ms(uint32_t milliseconds) { ticks+=milliseconds*33; }
uint32_t apx_spr_read(uint32_t spr) { return regs[spr]|(spr==0x88e0&&pm_busy?8:0); }
void apx_spr_write(uint32_t spr,uint32_t value) {
    if(spr==0x880c) {
        ++watchdog_writes;
        if(value==0x5500) { regs[spr]=0;stop_order=++order;return; }
        if(value==0x39) { assert(expect_event==1);event=1;longjmp(terminal,1); }
        assert((value&~7u)==0x50000);regs[spr]=0x18|(value&7);return;
    }
    if(spr==0x88e0) { if(!(value&0x100))pm_phase=0;regs[spr]=value&~8u;return; }
    if(spr==0x88ec) {
        assert(regs[0x88e0]&0x100);
        if(!pm_phase) { pm_command=value;pm_phase=1;return; }
        assert(pm_command>=0x40&&pm_command<=0x5b);
        if(pm_command&1) {
            ++pm_writes;pm[(pm_command-0x40)/2]=value;
            if(pm_command==0x41&&value==2) { assert(expect_event==2);event=2;longjmp(terminal,1); }
        } else regs[spr]=pm[(pm_command-0x40)/2];
        return;
    }
    regs[spr]=value;
    if(spr==0xa004)usb_order=++order;
}
uint32_t apx_storage_prepare_bootrom(void) { return storage_status; }
void apx_jpeg_shutdown(void) {}
void apx_audio_shutdown(void) {}
void apx_mic_shutdown(void) {}
void apx_camera_shutdown(void) {}
void apx_uart_shutdown(void) {}
void apx_spi_shutdown(void) {}
void apx_battery_shutdown(void) {}
int apx_clock_restore(void) {assert(usb_order>sd_order);return 0;}
void apx_clock_get_info(struct apx_clock_info *info) {memset(info,0,sizeof(*info));info->cpu_hz_estimate=144000000;}
void apx_display_shutdown(void) { display_order=++order; }
void apx_sd_shutdown(void) { sd_order=++order; }
void apx_buttons_shutdown(void) { ++order; }
int apx_usb_high_speed(void) { return 1; }
unsigned apx_usb_suspend_count(void) { return 0; }
unsigned apx_usb_resume_count(void) { return 0; }
void apx_usb_poll(void) { apx_maintenance(); }
int apx_usb_tx_idle(void) { return tx_idle; }
int apx_usb_receive(void *buffer,size_t size) {
    assert(size>=incoming_size);memcpy(buffer,incoming,incoming_size);
    int result=incoming_size;incoming_size=0;return result;
}
int apx_usb_send(const void *buffer,size_t size) {
    assert(size<=sizeof(outgoing));memcpy(outgoing,buffer,size);outgoing_size=size;
    tx_idle=0;ack_order=++order;return 0;
}
uint32_t apx_log_dropped(void) { return 0; }
size_t apx_log_read(void *buffer,size_t size) { (void)buffer;(void)size;return 0; }
_Noreturn void apx_bootrom(void) { assert(expect_event==3);event=3;longjmp(terminal,1); }
static void put32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;++i)p[i]=n>>(8*i); }
static uint32_t get32(const uint8_t *p) { uint32_t n=0;for(unsigned i=0;i<4;++i)n|=(uint32_t)p[i]<<(8*i);return n; }
static uint32_t request(unsigned command,const uint8_t *data,size_t size) {
    apx_debug_reset();tx_idle=1;memset(incoming,0,24);memcpy(incoming,"APXD",4);
    incoming[4]=1;incoming[5]=operation;incoming[7]=1;
    put32(incoming+8,1);put32(incoming+12,command);put32(incoming+16,size);
    if(size)memcpy(incoming+24,data,size);
    incoming_size=24+size;
    outgoing_size=0;apx_debug_poll();assert(outgoing_size>=24);return get32(outgoing+20);
}
int main(void) {
    regs[0x88e0]=0x220245;pm[0]=0x16;pm[1]=0x50;pm[10]=0x21;
    /* Cold SPI boot PM state, then the stock power-domain sequence. */
    {uint8_t saved[16];memcpy(saved,pm,sizeof(saved));uint32_t control=regs[0x88e0];
     memset(pm,0,sizeof(pm));pm[0]=0x02;pm[1]=0x58;pm[2]=0x10;pm[10]=0x01;regs[0x88e0]=0x220002;
     unsigned writes=pm_writes;
     assert(apx_power_domain_enable()==0&&pm_writes==writes+7);
     assert(pm[0]==0x16&&pm[1]==0xe0&&pm[2]==0x00&&pm[12]==0x08&&pm[13]==0x08&&pm[10]==0x21);
     assert((regs[0x88e0]&0x40)&&!(regs[0x88e0]&0x100));
     writes=pm_writes;assert(apx_power_domain_enable()==0&&pm[0]==0x16&&pm[12]==0x08&&pm[10]==0x21&&pm_writes==writes+6); /* idempotent: bit 5 already set, no extra write */
     regs[0x88e0]=0x220002|0x100; /* another transaction owns the bus: refuse, change nothing */
     writes=pm_writes;assert(apx_power_domain_enable()==-1&&pm_writes==writes);
     memcpy(pm,saved,sizeof(pm));regs[0x88e0]=control;pm_phase=0;}
    regs[0x880c]=0x1d;apx_watchdog_init();
    struct apx_watchdog_info info;apx_watchdog_get_info(&info);
    assert(info.enabled&&info.automatic&&info.selector==5&&info.inherited_control==0x1d);
    assert(apx_watchdog_start(6,APX_WATCHDOG_MANUAL)==0);
    unsigned before=watchdog_writes,polled=camera_polls;for(unsigned i=0;i<100;++i)apx_maintenance();
    assert(camera_polls==polled+100);
    assert(watchdog_writes==before&&regs[0x880c]==0x1e);
    assert(apx_watchdog_feed()==0&&watchdog_writes==before+1);
    assert(apx_watchdog_start(8,APX_WATCHDOG_AUTOMATIC)==-1&&watchdog_writes==before+1);
    assert(apx_watchdog_start(6,(enum apx_watchdog_mode)2)==-1&&watchdog_writes==before+1);
    apx_watchdog_stop();before=watchdog_writes;apx_maintenance();
    assert(regs[0x880c]==0&&watchdog_writes==before&&apx_watchdog_feed()==-1);
    assert(apx_watchdog_start(7,APX_WATCHDOG_AUTOMATIC)==0);before=watchdog_writes;
    apx_maintenance();assert(watchdog_writes==before+1&&regs[0x880c]==0x1f);
    uint8_t value;uint32_t original=regs[0x88e0];
    assert(apx_power_read(0x42,&value)==0&&value==0x50&&regs[0x88e0]==original);
    assert(apx_power_read(0x43,&value)==-1&&apx_power_read(0x3e,&value)==-1);
    regs[0x88e0]|=0x100;assert(apx_power_read(0x40,&value)==-1);regs[0x88e0]=original;
    pm_busy=1;assert(apx_power_read(0x40,&value)==-1&&!(regs[0x88e0]&0x100));pm_busy=0;
    uint8_t camera_payload[12]={0};operation=APXD_CAMERA;
    assert(request(1,camera_payload,4)==APXD_UNSUPPORTED);
    assert(request(2,camera_payload,12)==APXD_UNSUPPORTED);
    operation=APXD_SPI;assert(request(1,camera_payload,8)==APXD_UNSUPPORTED);
    operation=APXD_UART;assert(request(1,camera_payload,12)==APXD_UNSUPPORTED);
    operation=APXD_BATTERY;assert(request(0,NULL,0)==APXD_UNSUPPORTED);
    assert(request(0,camera_payload,4)==APXD_UNSUPPORTED);
    operation=APXD_SYSTEM;
    uint8_t config[8];put32(config,6);put32(config+4,0);
    assert(request(1,config,8)==0);before=watchdog_writes;
    tx_idle=1;apx_debug_poll();assert(watchdog_writes==before);
    assert(request(1,config,7)==APXD_INVALID);
    put32(config,8);assert(request(1,config,8)==APXD_RANGE);
    assert(request(4,config,1)==APXD_INVALID);
    storage_status=0x110;before=pm_writes;
    assert(request(4,NULL,0)==0x110&&request(5,NULL,0)==0x110&&pm_writes==before);
    tx_idle=1;apx_debug_poll();assert(!event);
    storage_status=0;pm_busy=1;assert(request(5,NULL,0)==APXD_IO);pm_busy=0;
    assert(apx_system_prepare((enum apx_system_action)99)==APXD_INVALID);
    for(unsigned action=1;action<=2;++action) {
        expect_event=action;event=0;
        assert(request(action==1?4:5,NULL,0)==0);
        apx_debug_poll();assert(!event); /* No shutdown before TX has drained. */
        assert(ack_order>0);
        if(!setjmp(terminal)) { tx_idle=1;apx_debug_poll();assert(0); }
        assert(event==action&&stop_order>ack_order&&display_order>stop_order&&sd_order>display_order&&usb_order>sd_order);
        assert(!(regs[0xa004]&0x40)&&!(regs[0x18]&2));
        if(action==2)assert(pm[10]==1&&pm[2]==0x10&&pm[1]==0xe0&&pm[0]==2);
        pm_phase=0;regs[0x88e0]=original;
    }
    puts("system model: watchdog auto/manual/disable, PM transaction bounds/timeouts, open-file refusal and ACK-before-reset/power-off passed");
}
