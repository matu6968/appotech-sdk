/* Host model of everything boot/flasher.c touches: the SPI controller with a 2 MiB NOR behind it, the PM chip's
 * serial bus, the watchdog and the data-cache maintenance hook. Built as a shared library together with the real
 * flasher.c (-DFLASHER_HOST) and driven from Python, so the payload logic and the host client are exercised
 * against each other without hardware. */
#include <stdint.h>
#include <string.h>

void rescue(volatile void *control);

#define NOR_SIZE 0x200000u
static uint8_t nor[NOR_SIZE];
static uint32_t spr[0x10000];
static uint8_t pm[0x60];
static unsigned pm_phase, pm_address;
static uint32_t wdt, wdt_reloads, busy_polls, busy_left;
static const uint8_t *sram_lo, *sram_hi;
#define SYNC_LOG 4096
static struct {uint64_t start; uint32_t bytes;} syncs[SYNC_LOG];
static unsigned sync_count;

/* SPI NOR state machine, fed one byte per controller transfer while chip select is low. */
static struct {unsigned count; uint8_t op; uint32_t address; uint8_t status; uint8_t page[256]; unsigned page_len;} flash;

static void flash_select(void){flash.count=0;flash.op=0;flash.address=0;flash.page_len=0;} /* status persists */
static void flash_deselect(void){
    if(flash.op==0x02&&flash.count>=4&&(flash.status&2)){
        uint32_t base=flash.address&~255u;
        for(unsigned i=0;i<flash.page_len;++i)nor[base+((flash.address+i)&255)]&=flash.page[i];
        flash.status&=~2u;flash.status|=1;busy_left=busy_polls;
    }
}
static uint8_t flash_byte(uint8_t out){
    uint8_t in=0xff;
    if(flash.count==0){flash.op=out;if(out==6)flash.status|=2;}
    else if(flash.op==0x05){in=flash.status;if(flash.status&1){if(busy_left)--busy_left;if(!busy_left)flash.status&=~1u;}}
    else if(flash.op==0x9f){static const uint8_t id[3]={0x5e,0x40,0x15};in=flash.count<=3?id[flash.count-1]:0xff;}
    else if(flash.op==0x03||flash.op==0x02||flash.op==0x20){
        if(flash.count<=3)flash.address=flash.address<<8|out;
        else if(flash.op==0x03)in=nor[(flash.address++)&(NOR_SIZE-1)];
        else if(flash.op==0x02&&flash.page_len<256)flash.page[flash.page_len++]=out;
        if(flash.op==0x20&&flash.count==3&&(flash.status&2)){
            memset(nor+(flash.address&~4095u),0xff,4096);flash.status&=~2u;flash.status|=1;busy_left=busy_polls;
        }
    }
    ++flash.count;
    return in;
}

uint32_t flasher_rd(unsigned r){
    if(r==0x880c)return wdt;
    if(r==0x9248)return 1; /* transfers complete instantly */
    if(r==0x88e0)return spr[r]&~8u; /* PM bus never busy */
    return spr[r&0xffff];
}
void flasher_wr(unsigned r,uint32_t v){
    if(r==0x880c){if(v==0x5500)wdt=0;else if(v==0x5001d&&wdt)++wdt_reloads;return;}
    if(r==0x9000){uint32_t was=spr[0x9000];spr[0x9000]=v;
        if((was&0x4000)&&!(v&0x4000))flash_select();
        if(!(was&0x4000)&&(v&0x4000))flash_deselect();
        return;}
    if(r==0x9244&&v==1){
        int receive=!!(spr[0x9000]&0x80);
        uint8_t in=flash_byte((uint8_t)spr[0x9250]);
        spr[0x9250]=receive?in:spr[0x9250];
        return;}
    if(r==0x88ec){
        if(pm_phase==0){pm_address=v&0xff;pm_phase=1;}
        else{pm_phase=0;if(pm_address&1)pm[(pm_address-1)%sizeof pm]=(uint8_t)v;else spr[0x88ec]=pm[pm_address%sizeof pm];}
        return;}
    if(r==0x88e0){if(!(v&0x100))pm_phase=0;}
    spr[r&0xffff]=v;
}
int flasher_is_sram(const volatile void *p){return (const uint8_t *)p>=sram_lo&&(const uint8_t *)p<sram_hi;}
void flasher_cache_range(const volatile void *start,uint32_t bytes){
    if(sync_count<SYNC_LOG){syncs[sync_count].start=(uint64_t)(uintptr_t)start;syncs[sync_count].bytes=bytes;}
    ++sync_count;
}

/* -- harness API (ctypes) -- */
void model_reset(void){
    memset(nor,0xff,sizeof nor);memset(spr,0,sizeof spr);memset(&flash,0,sizeof flash);
    for(unsigned i=0;i<sizeof pm;++i)pm[i]=0;
    pm[0x40]=0x16;pm[0x44]=0x00;pm[0x54]=0x21;pm[0x58]=0x0c;pm[0x5a]=0x00;
    spr[0x9000]=0x4000; /* chip select released */
    spr[0x1800]=1;      /* loader data cache on */
    pm_phase=0;wdt=0x1234;wdt_reloads=0;busy_polls=2;busy_left=0;sync_count=0;
}
uint8_t *model_nor(void){return nor;}
uint8_t *model_pm(void){return pm;}
void model_set_busy_polls(uint32_t n){busy_polls=n;}
void model_set_sram_window(const void *lo,uint64_t length){sram_lo=lo;sram_hi=(const uint8_t *)lo+length;}
void model_set_spr(unsigned r,uint32_t v){spr[r&0xffff]=v;}
uint32_t model_get_spr(unsigned r){return spr[r&0xffff];}
uint32_t model_wdt(void){return wdt;}
uint32_t model_wdt_reloads(void){return wdt_reloads;}
unsigned model_sync_count(void){return sync_count;}
void model_sync_get(unsigned i,uint64_t *start,uint32_t *bytes){*start=syncs[i].start;*bytes=syncs[i].bytes;}
void model_call(void *control){rescue(control);}
