/* Host model of the cold-boot flight recorder against a simulated NOR chip. */
#include <appotech/trace.h>
#include <appotech/soc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define CAPACITY 0x200000u
#define SECTOR APX_TRACE_SECTOR
static uint32_t regs[65536];
uint32_t trace_test_ring[64];
static uint8_t flash[CAPACITY];
static char uart[4096];
static unsigned uart_length;
static unsigned selected,position,spi_address,wel,busy_reads,stuck_busy,pending_erase,pending_program;
static unsigned stray_writes,erases,programs,gated,setups;
static uint8_t command,jedec[3]={0x5e,0x40,0x15};
static uint8_t spi_byte(uint8_t value,unsigned receive){
    assert(selected);
    if(!position++){command=value;spi_address=0;pending_erase=pending_program=0;if(command==6)wel=1;return 0;}
    if(command==0x9f){assert(receive&&position<=4);return jedec[position-2];}
    if(command==5){assert(receive);if(stuck_busy)return 1;if(busy_reads){--busy_reads;return 1;}return wel?2:0;}
    if(command==2||command==0x20||command==3){
        if(position<=4){spi_address=(spi_address<<8)|value;if(position==4){if(command==2)pending_program=1;if(command==0x20)pending_erase=1;}return 0;}
        assert(command==2||command==3);
        assert(spi_address<CAPACITY);
        if(command==3){assert(receive);return flash[spi_address++];}
        assert(wel);
        if(spi_address<0x1fe000)++stray_writes;
        flash[spi_address]&=value;
        /* Page wrap, as on the real chip. */
        spi_address=(spi_address&~255u)|((spi_address+1)&255u);
    }
    return 0xff;
}
uint32_t apx_spr_read(uint32_t r){if(r==0x908c)return 2;return regs[r];}
void apx_spr_write(uint32_t r,uint32_t value){
    uint32_t old=regs[r];regs[r]=value;
    if(r==0x9088&&uart_length<sizeof(uart)-1)uart[uart_length++]=(char)value;
    if(r==0x9000&&!(old&0x4000)&&(value&0x4000)&&selected){
        if(pending_erase&&position==4){
            assert(wel);++erases;
            if(spi_address!=SECTOR&&spi_address!=0x1fe000)++stray_writes;
            else memset(flash+spi_address,0xff,4096);
            wel=0;busy_reads=2;
        }
        if(pending_program){++programs;wel=0;busy_reads=1;}
        selected=0;
    }
    if(r==0x9000&&(old&0x4000)&&!(value&0x4000)){selected=1;position=0;}
    if(r==0x9000&&value==0x01115005&&gated){gated=0;++setups;}
    if(r==0x9244&&value==1&&gated){regs[0x9248]|=1;regs[0x9250]=0xff;return;}
    if(r==0x9244&&value==1){regs[0x9248]&=~1u;regs[0x9250]=spi_byte(regs[0x9250],regs[0x9000]&0x80);regs[0x9248]|=1;}
}
static void reset_chip(void){
    memset(flash,0x55,sizeof(flash));memset(flash+SECTOR,0xff,4096); /* vendor data elsewhere */
    memset(regs,0,sizeof(regs));regs[0x9000]=0x4000;
    uart_length=0;memset(uart,0,sizeof(uart));
    stray_writes=erases=programs=stuck_busy=busy_reads=wel=gated=setups=0;
}
static unsigned outside_unchanged(void){
    for(unsigned i=0;i<CAPACITY;++i)if((i<SECTOR||i>=SECTOR+4096)&&flash[i]!=0x55)return 0;
    return 1;
}
int main(void){
    /* Disabled until begin: nothing may be written. */
    reset_chip();
    apx_trace(APX_TRACE_ENTRY,1);apx_trace_regs(0);
    assert(!programs&&!erases&&!uart_length);

    /* Normal run. */
    reset_chip();
    regs[0x18]=0x1234;regs[0x8858]=0x1123;
    apx_trace_begin();
    assert(erases==1&&programs>=2&&!stray_writes);
    assert(!memcmp(flash+SECTOR,"APXT\1\0\0\0",8)&&!memcmp(flash+SECTOR+8,"model-build-id!!",16));
    const uint32_t *snap=(const uint32_t *)(flash+SECTOR+APX_TRACE_REGS_OFFSET);
    assert(snap[0]==0x18&&snap[1]==0x1234);
    unsigned found=0;for(unsigned i=0;i<APX_TRACE_SPR_COUNT;++i)if(snap[2*i]==0x8858){assert(snap[2*i+1]==0x1123);found=1;}
    assert(found&&snap[2*31]==0x9850&&snap[2*30]==0x984c); /* all 32 slots used */
    apx_trace(APX_TRACE_ENTRY,0x123456);
    apx_trace(0x42,0xabcdef99); /* arg truncates to 24 bits */
    const uint8_t *c=flash+SECTOR+APX_TRACE_CRUMBS_OFFSET;
    assert(!memcmp(c+4*1,"\x01\x56\x34\x12",4)&&!memcmp(c+4*0x42,"\x42\x99\xef\xcd",4));
    assert(c[4*2]==0xff&&c[4*0x43]==0xff);
    assert(strstr(uart,"T01:123456\n")&&strstr(uart,"T42:cdef99\n"));
    apx_trace(0,1);apx_trace(255,1);apx_trace(256,1);apx_trace(1000,1);
    assert(c[0]==0xff&&c[4*255]==0xff);
    apx_trace_regs(2);apx_trace_regs(4);apx_trace_regs(99);
    assert(((const uint32_t *)(flash+SECTOR+0x300))[0]==0x18);
    assert(*(const uint32_t *)(flash+SECTOR+0x400)==0xffffffffu);
    assert(!stray_writes&&outside_unchanged());

    /* A later boot starts clean. */
    apx_trace_begin();
    assert(erases==2&&c[4*1]==0xff&&c[4*0x42]==0xff);

    /* Same crumb twice only clears bits; the slot is still inside the sector. */
    apx_trace(0x30,0xffffff);apx_trace(0x30,0);
    assert(c[4*0x30]==0x30&&!stray_writes);

    /* Controller left gated by the ROM: the fallback setup brings it up. */
    reset_chip();gated=1;
    apx_trace_begin();apx_trace(5,5);
    assert(setups==1&&erases==1&&flash[SECTOR+APX_TRACE_CRUMBS_OFFSET+20]==5&&outside_unchanged());
    reset_chip();apx_trace_begin();
    assert(setups==0); /* no setup when the ROM state already works */

    /* Wrong chip: no erase, no program, stays off. */
    reset_chip();jedec[0]=0x00;
    apx_trace_begin();apx_trace(1,2);apx_trace_regs(0);
    assert(!erases&&!programs&&!uart_length&&outside_unchanged());
    jedec[0]=0x5e;

    /* A chip that never leaves busy cannot hang the boot and leaves tracing off. */
    reset_chip();stuck_busy=1;
    apx_trace_begin();apx_trace(1,2);
    assert(!programs&&!uart_length&&outside_unchanged());

    /* After a failed begin a stale enable from an earlier boot must not matter. */
    reset_chip();apx_trace_begin();apx_trace(7,7);
    assert(flash[SECTOR+APX_TRACE_CRUMBS_OFFSET+28]==7&&outside_unchanged());
    /* A second log in another sector leaves the first one alone. */
    reset_chip();
    apx_trace_begin();apx_trace(0x21,1);
    uint8_t first[4096];memcpy(first,flash+SECTOR,4096);
    memset(flash+0x1fe000,0xff,4096);
    apx_trace_begin_at(APX_TRACE_LAUNCH_SECTOR);apx_trace(0x44,0x55);
    assert(!memcmp(first,flash+SECTOR,4096)&&flash[0x1fe000+APX_TRACE_CRUMBS_OFFSET+4*0x44]==0x44&&!stray_writes);
    apx_trace_begin();assert(flash[SECTOR+APX_TRACE_CRUMBS_OFFSET+4*0x21]==0xff&&flash[0x1fe000+APX_TRACE_CRUMBS_OFFSET+4*0x44]==0x44);
    /* Modes: UART-only never touches the flash bus; flash-only prints nothing; off records nothing. */
    reset_chip();
    apx_trace_begin_mode(APX_TRACE_LAUNCH_SECTOR,2);
    unsigned p0=programs,e0=erases,u0=uart_length;
    apx_trace(0x10,0x123456);apx_trace_regs(1);
    assert(programs==p0&&erases==e0&&uart_length>u0&&strstr(uart,"T10:123456\n")&&apx_trace_active());
    reset_chip();
    apx_trace_begin_mode(APX_TRACE_LAUNCH_SECTOR,1);
    u0=uart_length;apx_trace(0x11,7);
    assert(uart_length==u0&&programs>1&&flash[0x1fe000+APX_TRACE_CRUMBS_OFFSET+4*0x11]==0x11);
    apx_trace_off();p0=programs;u0=uart_length;apx_trace(0x12,1);
    assert(programs==p0&&uart_length==u0&&!apx_trace_active());
    /* SRAM ring: a "crashed" run leaves crumbs in on-chip SRAM; the next boot imports them into its log. */
    reset_chip();
    apx_trace_begin_mode(APX_TRACE_LAUNCH_SECTOR,4);
    p0=programs;e0=erases;u0=uart_length;
    apx_trace(0x01,0xa00000);apx_trace(0x05,0x1d);apx_trace(0x70,0x123456);
    assert(programs==p0&&erases==e0&&uart_length==u0&&trace_test_ring[1]==3);
    /* ... the run dies here; the next SPI boot starts its own log and imports the ring ... */
    apx_trace_begin();
    apx_trace_import_ram();
    const uint8_t *r=flash+SECTOR+APX_TRACE_CRUMBS_OFFSET;
    assert(!memcmp(r+4*0xc0,"\xc0\x00\x00\x01",4)&&!memcmp(r+4*0xc1,"\xc1\x1d\x00\x05",4)&&!memcmp(r+4*0xc2,"\xc2\x56\x34\x70",4));
    assert(r[4*0xa0]==0xa0&&r[4*0xa2]==0xa2&&r[4*0xe0]==0xe0&&r[4*0xe1]==0xe1); /* timer + heartbeat slots */
    assert(r[4*0xc3]==0xff&&trace_test_ring[0]==0); /* imported once, then consumed */
    apx_trace_import_ram();assert(r[4*0xc3]==0xff);
    /* a boot with no ring leaves the log alone */
    reset_chip();apx_trace_begin();apx_trace_import_ram();assert(flash[SECTOR+APX_TRACE_CRUMBS_OFFSET+4*0xc0]==0xff);
    puts("trace model ok");
    return 0;
}