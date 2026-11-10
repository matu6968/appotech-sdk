#define _GNU_SOURCE
#include <appotech/sd.h>
#include <appotech/soc.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ff.h>
#include <diskio.h>
#include "board.h"
static uint32_t registers[65536],ticks,polls;
static unsigned high_capacity,present=1,crc_error,stuck,data_stuck,dma_armed;
static unsigned write_crc_error,busy_stuck,status_stuck,r1_write_protected,cleaned,pending_write;
/* open-ended write (CMD25): wstreaming = the card is receiving blocks; busy_pending = the last block's programming has not been waited for (a block armed then is a driver bug);
 * write_token_once/skip = the data-response token of a chosen block is "CRC error" instead of "accepted" */
static unsigned wstreaming,busy_pending,write_token_once,write_token_skip;
static unsigned read_crc_skip,read_crc_once,write_crc_once,replace_on_error,cid_generation;
static uint32_t write_sector,busy_until;
/* open-ended read (CMD18) state, the CMD6 high-speed model and command bookkeeping */
static unsigned arm_reads,streaming,hs_mode,hs_switched,cmd6_count,cmds[64],stream_ended_at_end,end_cmd12_error;
static uint32_t stream_next,cmd6_args[16],id_divider_seen;
static uintptr_t last_dma;
static unsigned landed_in_caller,landed_in_driver; /* where delivered blocks went: the test's RAM window (the caller's buffer) or a driver block */
/* four-bit bus: bus4 = the card's width (ACMD6), the controller's width is CONTROL bit 2; app = the previous command was CMD55;
 * wide_mode: 0 everything works, 1 ACMD6 is refused (R1 error), 2 DAT lines in dat_stuck_lines deliver nothing in four-bit mode,
 * 3 the four-bit status arrives with a controller CRC error, 4 it never arrives, 5 the one-bit status never arrives, 6 the card keeps reporting DAT_BUS_WIDTH 00 */
static unsigned bus4,app,wide_mode,dat_stuck_lines,acmd6_count,acmd13_count,acmd6_args_bad;
/* a card that is deaf for a while after a failed four-bit read (seen on hardware: CMD8 and CMD0 time out, then it listens again): no command is answered until wide_busy_ticks have passed since the failed read */
static uint32_t wide_busy_ticks,wide_error_tick;
static unsigned wide_deaf; /* set by a failed four-bit read: the deaf window runs from then */
/* hs_mode: 0 the card has no CMD6 (CSD command class 10 clear), 1 high speed works, 2 the check reports the function not
 * selectable, 3 the status block arrives with a controller CRC error (CONTROL bit 11) */
static struct { uint32_t sector;unsigned valid;uint8_t data[512]; } blocks[1024];
static uint8_t csd[16],sram[16];
static uint32_t rca=0x12340000;
static void set_bits(unsigned high,unsigned low,uint32_t value) {
    for(unsigned b=low;b<=high;++b) {
        unsigned mask=1u<<(b%8),index=(127-b)/8;
        csd[index]=(csd[index]&~mask)|((value>>(b-low)&1u)?mask:0);
    }
}
uint32_t apx_ticks(void) { return ticks++; }
static unsigned jump_in_maintenance;
void apx_sd_poll(void);
void apx_maintenance(void) { if(jump_in_maintenance)ticks+=2000; /* a slow maintenance step */ apx_sd_poll(); }
void apx_usb_poll(void) { ++polls; }
int apx_dma_clean(const void *buffer,size_t length) {
    assert(!((uintptr_t)buffer&15)&&length&&!(length&15)&&length<=(1u<<20));cleaned=1;return 0;
}
uint8_t apx_mmio_read8(uintptr_t address) { assert(address>=0x2000&&address<0x2010);return sram[address-0x2000]; }
int apx_dma_invalidate(void *buffer,size_t length) {
    assert(!((uintptr_t)buffer&15)&&length&&!(length&15));return 0;
}
static void deliver_block(uint32_t sector) { /* the card's next block into the armed DMA */
    assert(dma_armed&&registers[0x9894]==512);dma_armed=0;
    assert(((registers[0x9880]>>2)&1u)==bus4);
    last_dma=registers[0x9890];
    if(last_dma>=0x021a0000u&&last_dma<0x021b0000u)++landed_in_caller;else ++landed_in_driver;
    if(!data_stuck) {
        uint8_t *out=(void *)(uintptr_t)registers[0x9890];
        for(unsigned i=0;i<512;++i)out[i]=(sector*97+i)&255;
        for(unsigned i=0;i<1024;++i)if(blocks[i].valid&&blocks[i].sector==sector)memcpy(out,blocks[i].data,512);
        registers[0x9880]|=0x8000;
    }
    if(streaming&&sector+1>=apx_sd_get_info()->sectors)stream_ended_at_end=1; /* it just sent the last block of the card */
    unsigned bad=crc_error;
    if(read_crc_once) { if(read_crc_skip)--read_crc_skip;else { bad=1;--read_crc_once; } } /* the error lands on a chosen delivery */
    if(bad&&replace_on_error){++cid_generation;replace_on_error=0;}
    {static const unsigned noise[4]={0x400000,0x000000,0x200000,0x600000};static unsigned n;
     /* Good reads report varying bits 22:21 (hardware noise); only bit 16 flags an error. */
     registers[0x9880]=(registers[0x9880]&~0x710000u)|(bad?0x10000u:noise[n++&3]);}
}
uint32_t apx_spr_read(uint32_t address) {
    if(address==0x9884)return registers[address]|(registers[0x9880]&0x28000);
    /* a running stream hands over its next block as soon as the DMA is armed */
    /* ...but not at once: the block takes a few status reads to arrive, so code that does not wait for it is caught */
    if(address==0x9880&&streaming&&dma_armed&&!(registers[0x9880]&0x8000)&&++arm_reads>=3)deliver_block(stream_next++);
    if(address==0x9880&&(registers[address]&2)&&!busy_stuck&&(int32_t)(ticks-busy_until)>=0) {
        registers[address]|=1u<<20;busy_pending=0;
    }
    return registers[address];
}
static void execute(void) {
    unsigned cmd=registers[0x9898]&63;uint32_t argument=registers[0x989c];
    assert(cmd==0||cmd==2||cmd==3||cmd==6||cmd==7||cmd==8||cmd==9||cmd==12||cmd==13||cmd==16||cmd==17||cmd==18||cmd==24||cmd==25||cmd==41||cmd==55);
    ++cmds[cmd];
    const unsigned was_app=app;app=0;
    registers[0x9880]&=~((1u<<17)|(1u<<19)|0x800u);
    if(stuck)return;
    registers[0x9880]|=1u<<17;
    if(wide_deaf&&wide_busy_ticks&&(int32_t)(ticks-wide_error_tick)<(int32_t)wide_busy_ticks) { registers[0x9880]|=1u<<19;registers[0x989c]=0;return; } /* deaf */
    if(!present) { registers[0x9880]|=1u<<19;registers[0x989c]=0;return; }
    uint32_t reply=0x900;
    switch(cmd) {
    case 0:if(id_divider_seen==0xffff)id_divider_seen=registers[0x9888];streaming=wstreaming=busy_pending=pending_write=0;bus4=0;reply=0;break;
    case 8:assert(argument==0x1aa);reply=argument;break;
    case 55:app=1;reply=0x120;break;
    case 41:reply=0x80ff8000|(high_capacity?0x40000000:0);
        break;
    case 3:reply=rca|0x520;break;
    case 2:case 9:
        for(unsigned i=0;i<16;++i)sram[i^3]=cmd==9?csd[i]:(uint8_t)(i+cid_generation);
        break;
    case 13:if(was_app) { /* ACMD13: the 64-byte SD status, on as many lines as the card is set to. Its first byte holds DAT_BUS_WIDTH (bits 7:6). */
            assert(!streaming&&!wstreaming&&dma_armed&&registers[0x9894]==64);dma_armed=0;++acmd13_count;
            if(bus4&&wide_mode!=0) { wide_error_tick=ticks;wide_deaf=1; }
            assert(((registers[0x9880]>>2)&1u)==bus4); /* the controller and the card must agree about the width before data flows */
            uint8_t *out=(void *)(uintptr_t)registers[0x9890];
            out[0]=(uint8_t)((bus4&&wide_mode!=6)?0x80:0);
            for(unsigned i=1;i<64;++i)out[i]=(uint8_t)((i*37+11)|0x6e); /* bits 1, 2, 3, 5, 6 always set: every extra line carries data in every byte */
            if(bus4)for(unsigned i=0;i<64;++i)for(unsigned line=1;line<4;++line)if(dat_stuck_lines&(1u<<line))out[i]&=(uint8_t)~((1u<<line)|(1u<<(line+4)));
            unsigned deliver=1;
            if(bus4&&wide_mode==3)registers[0x9880]|=0x800;
            if(bus4&&wide_mode==4)deliver=0;
            if(!bus4&&wide_mode==5)deliver=0;
            if(deliver)registers[0x9880]|=0x8000;
            break;
        }
        /* fall through */
    case 7:assert(argument==rca);if(cmd==13&&status_stuck)reply=0xe00;
        if(cmd==13&&wstreaming)reply=0xd00; /* state 6: receiving data: a driver that polls CMD13 with the write stream open waits forever */
        if(cmd==13&&streaming)reply=0xb00; /* state 5: sending data. A host that polls for "transfer" with the stream open waits forever. */
        break;
    case 16:assert(!high_capacity&&argument==512);break;
    case 6: { /* SWITCH_FUNC: group 1 function 1 = high speed; a 64-byte status block */
        if(was_app) { /* ACMD6, SET_BUS_WIDTH: 0 = one bit, 2 = four bits; the controller must still be one bit wide when the card changes */
            assert(!streaming&&!wstreaming&&!dma_armed&&!busy_pending);
            ++acmd6_count;if(argument!=0&&argument!=2)++acmd6_args_bad;
            assert(!(registers[0x9880]&4u));
            if(wide_mode==1)reply|=1u<<22; /* ILLEGAL_COMMAND */
            else bus4=argument==2;
            break;
        }
        assert(!streaming&&dma_armed&&registers[0x9894]==64);dma_armed=0;
        assert(((registers[0x9880]>>2)&1u)==bus4);
        assert(cmd6_count<16);cmd6_args[cmd6_count++]=argument;
        uint8_t *out=(void *)(uintptr_t)registers[0x9890];memset(out,0,64);
        out[16]=hs_mode==2?15:1; /* the function the card would select (or does select) in group 1 */
        if((argument>>31)&&hs_mode!=2)hs_switched=1;
        if(hs_mode==3)registers[0x9880]|=0x800;
        registers[0x9880]|=0x8000;
        break;
    }
    case 12:assert((streaming&&!dma_armed)||(wstreaming&&!busy_pending)); /* a stream is stopped between blocks, never with a receive armed (read-ahead must finish first) or a block still programming */
        if(wstreaming)busy_pending=1; /* the card programs what it has received: busy until the driver has waited for it */
        streaming=wstreaming=pending_write=0;registers[0x9880]&=~(1u<<20);busy_until=ticks+16; /* R1b */
        /* Some cards flag OUT_OF_RANGE (bit 31) in this R1 when the stream ran to the end of the card. */
        if(stream_ended_at_end&&end_cmd12_error)reply|=1u<<31;
        stream_ended_at_end=0;
        break;
    case 17:
        assert(!streaming&&!wstreaming&&!busy_pending);assert(registers[0x9894]==512);
        if(!high_capacity)assert(argument%512==0);
        deliver_block(high_capacity?argument:argument/512);
        break;
    case 18: { /* the first block arrives with the command, the rest as the DMA is re-armed */
        assert(!streaming&&!wstreaming&&!busy_pending&&registers[0x9894]==512);
        if(!high_capacity)assert(argument%512==0);
        uint32_t sector=high_capacity?argument:argument/512;
        streaming=1;stream_next=sector+1;
        deliver_block(sector);
        break;
    }
    case 24:
        assert(!streaming&&!wstreaming&&!dma_armed&&!busy_pending&&cleaned);
        write_sector=high_capacity?argument:argument/512;
        if(!high_capacity)assert(argument%512==0);
        if(r1_write_protected)reply|=1u<<26;
        else pending_write=1;
        break;
    case 25: /* open-ended write: blocks follow as the transmit DMA is armed, each answered with a data-response token and busy */
        assert(!streaming&&!wstreaming&&!dma_armed&&!busy_pending&&cleaned);
        write_sector=high_capacity?argument:argument/512;
        if(!high_capacity)assert(argument%512==0);
        if(r1_write_protected)reply|=1u<<26;
        else { pending_write=1;wstreaming=1; }
        break;
    }
    registers[0x989c]=reply;
}
void apx_spr_write(uint32_t address,uint32_t value) {
    if(address==0x9894&&!value)dma_armed=0;
    if(address==0x9884) {
        if((value&0x10)&&registers[0x9894]) { dma_armed=1;arm_reads=0; }
        registers[0x9880]&=~(value&0x28000);
        registers[address]=value&~(0x28000|0x10u); /* bit 4 triggers a receive and is not kept: a later read-modify-write must not re-arm */
        if((value&0x30)==0x30&&pending_write) {
            assert(registers[0x9894]==512&&dma_armed&&!busy_pending&&cleaned);dma_armed=0;
            assert(((registers[0x9880]>>2)&1u)==bus4);
            if(!wstreaming)pending_write=cleaned=0;
            registers[0x9880]&=~((1u<<20)|(1u<<16)|0x600000u);busy_until=ticks+32;busy_pending=1;
            if(!data_stuck)registers[0x9880]|=1u<<15;
            unsigned token_bad=0;
            if(write_token_once) { if(write_token_skip)--write_token_skip;else { token_bad=1;--write_token_once; } }
            registers[0x9880]|=token_bad?0x200000u:0x400000u; /* data-response token in bits 22:21: 10b = accepted, 01b = CRC error */
            if(write_crc_error||write_crc_once){registers[0x9880]|=1u<<16;if(write_crc_once)--write_crc_once;}
            else if(!data_stuck&&!token_bad) {
                unsigned i;for(i=0;i<1024;++i)if(!blocks[i].valid||blocks[i].sector==write_sector)break;
                assert(i<1024);blocks[i].valid=1;blocks[i].sector=write_sector;
                memcpy(blocks[i].data,(void *)(uintptr_t)registers[0x9890],512);
            }
            if(wstreaming)++write_sector;
        }
        if(value==1||value==2||value==3||value==5)execute();
    } else registers[address]=value;
}
static void check_pattern(const uint8_t *p,uint32_t first,unsigned count) {
    for(unsigned s=0;s<count;++s)for(unsigned i=0;i<512;++i)assert(p[s*512+i]==((first+s)*97+i)%256);
}
/* Open-ended reads: one CMD18 serves many calls, isolated sectors stay CMD17, and everything else stops the stream first. */
static void streaming_tests(uint8_t *ram) {
    memset(cmds,0,sizeof(cmds));memset(ram,0x5a,8192);
    /* a multi-sector read into an aligned buffer: one CMD18, the DMA straight into the caller's memory */
    landed_in_caller=landed_in_driver=0;
    assert(apx_sd_read(300,ram,8)==0);
    assert(cmds[18]==1&&cmds[17]==0&&cmds[12]==0&&apx_sd_get_info()->streams==1);
    check_pattern(ram,300,8);
    /* every block of the call went straight into the caller's memory (no read-ahead block, no copy); only sector 308, the one after the call, was requested into a
     * driver block, and is already on its way (the DMA is armed for it) while no call is running */
    assert(landed_in_caller==8&&landed_in_driver<=1);
    for(unsigned i=8*512;i<8192;++i)assert(ram[i]==0x5a); /* the DMA never ran past the caller's last sector */
    assert(registers[0x9890]<(uintptr_t)ram||registers[0x9890]>=(uintptr_t)ram+8192);
    assert(streaming&&(dma_armed?stream_next==308:stream_next==309&&(registers[0x9880]&0x8000))); /* armed, or already delivered and waiting */
    /* the stream is still open: the next sector continues it with no command at all */
    assert(apx_sd_read(308,ram,1)==0);assert(cmds[18]==1&&cmds[17]==0&&cmds[12]==0&&streaming);check_pattern(ram,308,1);
    assert(dma_armed?stream_next==309:stream_next==310&&(registers[0x9880]&0x8000)); /* and 309 is on its way */
    /* sector after sector: one command in all, the card's data always arrives into the read-ahead block */
    landed_in_caller=landed_in_driver=0;
    for(uint32_t s=309;s<349;++s) { assert(apx_sd_read(s,ram,1)==0);check_pattern(ram,s,1); }
    assert(landed_in_driver>=39&&landed_in_caller==0); /* a caller that reads one sector at a time is served from the read-ahead block, the next one already on its way */
    assert(cmds[18]==1&&cmds[17]==0&&cmds[12]==0&&streaming);
    /* an isolated sector elsewhere stops it (CMD12) and is a plain CMD17 */
    assert(apx_sd_read(900,ram,1)==0);assert(cmds[12]==1&&cmds[17]==1&&cmds[18]==1&&!streaming);check_pattern(ram,900,1);
    assert(apx_sd_read(5,ram,1)==0);assert(cmds[12]==1&&cmds[17]==2); /* nothing was open, nothing to stop */
    /* a read that continues the previous one starts a stream, and the one after that needs no command */
    assert(apx_sd_read(700,ram,1)==0&&cmds[17]==3&&!streaming);
    assert(apx_sd_read(701,ram,1)==0&&cmds[18]==2&&cmds[17]==3&&streaming);
    assert(apx_sd_read(702,ram,1)==0&&cmds[18]==2&&cmds[17]==3&&cmds[12]==1);check_pattern(ram,702,1);
    /* an unaligned destination goes through the bounce block, not straight into the caller's memory */
    memset(ram,0x5a,8192);
    assert(apx_sd_read(703,ram+1,2)==0&&cmds[18]==2&&cmds[12]==1);check_pattern(ram+1,703,2);
    assert(last_dma<(uintptr_t)ram||last_dma>=(uintptr_t)ram+8192);
    assert(ram[0]==0x5a&&ram[1025]==0x5a); /* nothing outside the two sectors was touched */
    /* a write, a sync, and a clock change each stop an open stream first (the model rejects CMD24 mid-stream and reports
     * "sending data" to CMD13, so a driver that forgot would fail or time out here) */
    assert(apx_sd_read(1200,ram,4)==0&&streaming);assert(apx_sd_write(10,ram,1)==0&&!streaming);
    assert(apx_sd_read(1300,ram,2)==0&&streaming);assert(apx_sd_sync()==0&&!streaming);
    assert(apx_sd_read(1400,ram,2)==0&&streaming);assert(!apx_sd_set_clock(12000000)&&!streaming&&apx_sd_get_info()->clock_hz==12000000);
    assert(!apx_sd_set_clock(24000000));
    /* after a stop the next sequential read starts a fresh stream */
    uint32_t streams=apx_sd_get_info()->streams;
    assert(apx_sd_read(1402,ram,1)==0&&apx_sd_get_info()->streams==streams+1&&streaming);
    /* a card error in the middle of a stream is retried transparently from the failing sector */
    assert(apx_sd_read(1500,ram,2)==0&&streaming);
    uint32_t retries=apx_sd_get_info()->retries;
    read_crc_once=1;read_crc_skip=1; /* 1502 was requested by the read-ahead and is still on its way: it passes, 1503 is bad */
    assert(apx_sd_read(1502,ram,3)==0);
    assert(apx_sd_get_info()->retries==retries+1&&apx_sd_get_info()->initialized);check_pattern(ram,1502,3);
    /* an error in a block the read-ahead fetched is found by the next call, which retries it from there */
    assert(apx_sd_read(1600,ram,2)==0&&streaming);retries=apx_sd_get_info()->retries;
    read_crc_once=1;read_crc_skip=1; /* 1602 (on its way) passes, its own read-ahead 1603 arrives bad */
    assert(apx_sd_read(1602,ram,1)==0&&streaming);
    assert(apx_sd_get_info()->retries==retries);
    assert(apx_sd_read(1603,ram,1)==0);
    assert(apx_sd_get_info()->retries==retries+1&&apx_sd_get_info()->initialized);check_pattern(ram,1603,1);
    /* the end of the card: a stream must never run into the last block (a card may end it itself and flag OUT_OF_RANGE on the
     * CMD12, which the driver would take for a failure and shut the card down) */
    {
        const uint32_t total=(uint32_t)apx_sd_get_info()->sectors;
        const uint32_t errors0=apx_sd_get_info()->errors,retries0=apx_sd_get_info()->retries; /* earlier steps provoked errors on purpose */
        end_cmd12_error=1;
        assert(apx_sd_read(total-8,ram,8)==0&&apx_sd_get_info()->initialized);check_pattern(ram,total-8,8);
        assert(apx_sd_sync()==0&&apx_sd_get_info()->initialized&&apx_sd_get_info()->errors==errors0);
        assert(apx_sd_read(total-3,ram,1)==0&&apx_sd_read(total-2,ram,1)==0&&apx_sd_read(total-1,ram,1)==0&&apx_sd_get_info()->initialized);
        check_pattern(ram,total-1,1);
        assert(apx_sd_sync()==0&&apx_sd_get_info()->initialized&&apx_sd_get_info()->errors==errors0&&apx_sd_get_info()->retries==retries0);
        assert(apx_sd_read(total-1,ram,1)==0&&apx_sd_read(0,ram,1)==0&&apx_sd_get_info()->initialized);
        end_cmd12_error=0;
    }
    /* shutdown drops the stream; a read then reports not-ready instead of talking to the card */
    assert(apx_sd_read(1600,ram,2)==0&&streaming);apx_sd_shutdown();
    assert(apx_sd_read(1602,ram,1)==APX_SD_NOT_READY);
}
/* Open-ended writes (CMD25): a multi-sector or sequential write is one command and then only a DMA arm and a busy wait per sector; everything else closes it. */
static void fill_pattern(uint8_t *p,uint32_t first,unsigned count) {
    for(unsigned s=0;s<count;++s)for(unsigned i=0;i<512;++i)p[s*512+i]=(uint8_t)((first+s)*131+i*7+1);
}
static void check_written(uint8_t *ram,uint32_t first,unsigned count) { /* read back through the driver (which closes any write stream first) and compare */
    uint8_t *back=ram+16384;
    memset(back,0,count*512);
    assert(apx_sd_read(first,back,count)==0);
    for(unsigned s=0;s<count;++s)for(unsigned i=0;i<512;++i)assert(back[s*512+i]==(uint8_t)((first+s)*131+i*7+1));
}
static void write_stream_tests(uint8_t *ram) {
    memset(cmds,0,sizeof(cmds));
    /* several sectors: one CMD25, left open; the next sector continues it with no command */
    fill_pattern(ram,2000,8);
    assert(apx_sd_write(2000,ram,8)==0);
    assert(cmds[25]==1&&cmds[24]==0&&cmds[12]==0&&wstreaming&&apx_sd_get_info()->wstreams==1&&apx_sd_get_info()->writes==8);
    fill_pattern(ram,2008,1);assert(apx_sd_write(2008,ram,1)==0);
    assert(cmds[25]==1&&cmds[24]==0&&cmds[12]==0&&wstreaming&&apx_sd_get_info()->writes==9);
    assert((apx_sd_get_info()->last_write_status&0x600000u)==0x400000u); /* the accepted token was seen */
    /* a read closes the stream (CMD12, then back in the transfer state) before it asks the card for anything; the data is all there */
    check_written(ram,2000,9);
    assert(cmds[12]==1&&!wstreaming&&streaming); /* (the read-back left its own read stream open) */
    /* an isolated sector elsewhere is one CMD24, and it closes an open stream first */
    fill_pattern(ram,3000,3);assert(apx_sd_write(3000,ram,3)==0&&wstreaming&&!streaming&&cmds[25]==2&&cmds[12]==2); /* closed the read stream */
    fill_pattern(ram,5000,1);assert(apx_sd_write(5000,ram,1)==0&&!wstreaming&&cmds[24]==1&&cmds[25]==2&&cmds[12]==3);
    check_written(ram,5000,1);check_written(ram,3000,3);
    /* single sectors one after the other: the first is CMD24, the second starts a stream, the rest need no command */
    memset(cmds,0,sizeof(cmds));
    for(uint32_t sec=6000;sec<6012;++sec) { fill_pattern(ram,sec,1);assert(apx_sd_write(sec,ram,1)==0); }
    assert(cmds[24]==1&&cmds[25]==1&&cmds[12]==1&&wstreaming); /* (the one CMD12 closed the read stream left by the check above) */
    assert(apx_sd_sync()==0&&!wstreaming&&cmds[12]==2);
    assert(apx_sd_sync()==0&&cmds[12]==2); /* nothing open: nothing to close */
    check_written(ram,6000,12);
    /* an unaligned source goes through the bounce block; the data still arrives intact */
    fill_pattern(ram+1,7000,5);assert(apx_sd_write(7000,ram+1,5)==0);
    {   uint8_t *back=ram+16384;memset(back,0,2560);assert(apx_sd_read(7000,back,5)==0);
        for(unsigned s=0;s<5;++s)for(unsigned i=0;i<512;++i)assert(back[s*512+i]==(uint8_t)((7000+s)*131+i*7+1)); }
    /* the idle poll closes a stream nobody continues, but not at once and not while a call is running */
    fill_pattern(ram,8000,2);assert(apx_sd_write(8000,ram,2)==0&&wstreaming);
    unsigned closed=cmds[12];apx_sd_poll();assert(wstreaming&&cmds[12]==closed);
    ticks+=1700; /* the idle limit is 50 ms = 1638 ticks */apx_sd_poll();assert(!wstreaming&&cmds[12]==closed+1);
    check_written(ram,8000,2);
    /* slow maintenance (ticks jump by more than the idle limit inside every wait): the poll runs inside the call and must leave the card alone */
    jump_in_maintenance=1;fill_pattern(ram,8200,40);assert(apx_sd_write(8200,ram,40)==0&&wstreaming);jump_in_maintenance=0;
    check_written(ram,8200,40);
    /* a clock change closes it too (the card must be idle) */
    fill_pattern(ram,8100,2);assert(apx_sd_write(8100,ram,2)==0&&wstreaming);
    assert(!apx_sd_set_clock(12000000)&&!wstreaming);assert(!apx_sd_set_clock(24000000));
    /* the data-response token: a block the card refuses is retried; in the middle of a stream the blocks before it are not repeated */
    {   unsigned retries=apx_sd_get_info()->retries;
        fill_pattern(ram,9000,6);write_token_once=1;write_token_skip=2; /* the third block is answered "CRC error" */
        assert(apx_sd_write(9000,ram,6)==0);
        assert(apx_sd_get_info()->retries==retries+1&&apx_sd_get_info()->initialized);
        check_written(ram,9000,6); }
    /* a card that keeps refusing: the call fails after the allowed retries and the card is stopped */
    fill_pattern(ram,9100,3);write_token_once=100;write_token_skip=0;
    assert(apx_sd_write(9100,ram,3)==APX_SD_CARD_ERROR&&!apx_sd_get_info()->initialized);
    write_token_once=0;assert(apx_sd_init()==0);
    /* a write-protected card (R1 flag on CMD25) is refused and never retried */
    r1_write_protected=1;fill_pattern(ram,9200,2);assert(apx_sd_write(9200,ram,2)==APX_SD_WRITE_PROTECTED);r1_write_protected=0;
    assert(apx_sd_get_info()->write_protected);
    assert(apx_sd_init()==0);
    /* the last sector of the card and a multi-sector write that ends at it */
    {   const uint32_t total=(uint32_t)apx_sd_get_info()->sectors;
        fill_pattern(ram,total-4,4);assert(apx_sd_write(total-4,ram,4)==0);assert(apx_sd_sync()==0);check_written(ram,total-4,4); }
}
/* After init: four bits (card, controller, pads) or one bit with every borrowed pad back where it was. */
static void check_pads(unsigned wide) {
    if(wide) {
        assert((registers[0x8100]&8)==8&&(registers[0x8110]&0xc00)==0xc00&&(registers[0x8080]&8)==8&&(registers[0x8090]&0xc00)==0xc00);
        assert((registers[0x80c0]&8)==0&&(registers[0x80d0]&0xc00)==0&&(registers[0x8040]&8)==8&&(registers[0x8050]&0xc00)==0xc00);
    } else {
        assert((registers[0x8100]&8)==0&&registers[0x8110]==0x0400&&(registers[0x8080]&8)==0&&registers[0x8090]==0x0800);
        assert((registers[0x80c0]&8)==8&&registers[0x80d0]==0x0c00&&(registers[0x8040]&8)==0&&registers[0x8050]==0);
    }
}
static void check_width(unsigned wide) {
    const struct apx_sd_info *info=apx_sd_get_info();
    assert(info->initialized);
    assert(info->bus_width==(wide?4u:1u));
    assert(bus4==wide&&((registers[0x9880]>>2)&1u)==wide);
    assert(info->bus_probe==(wide?APX_SD_WIDE_OK:wide_mode==0?APX_SD_WIDE_NOT_TRIED:info->bus_probe));
    check_pads(wide);
    assert(acmd6_args_bad==0);
}
static void initialize(unsigned hc) {
    high_capacity=hc;present=1;crc_error=stuck=data_stuck=0;memset(csd,0,16);
    write_crc_error=busy_stuck=status_stuck=r1_write_protected=cleaned=pending_write=wstreaming=busy_pending=write_token_once=write_token_skip=0;
    read_crc_once=write_crc_once=replace_on_error=cid_generation=0;
    memset(blocks,0,sizeof(blocks));busy_until=0;
    set_bits(127,126,hc?1:0);
    set_bits(95,84,hs_mode?0x5f5:0x1f5); /* command classes; class 10 (bit 10) = CMD6 */
    streaming=hs_switched=cmd6_count=stream_ended_at_end=end_cmd12_error=0;id_divider_seen=0xffff;memset(cmd6_args,0,sizeof(cmd6_args));
    if(hc)set_bits(69,48,65535); /* 32 GiB */
    else { set_bits(83,80,10);set_bits(73,62,3771);set_bits(49,47,7); }
    registers[0x4800]=0xffffffff;registers[0x8040]=0xfffffff7;
    /* the pads the four-bit bus borrows (PA3, PE10/PE11) start in a known state, so that giving them back can be checked */
    registers[0x8100]=0;registers[0x8110]=0x0400;registers[0x8080]=0;registers[0x8090]=0x0800;registers[0x80c0]=0x0008;registers[0x80d0]=0x0c00;registers[0x8050]=0;
    bus4=app=acmd6_count=acmd13_count=acmd6_args_bad=wide_deaf=0;memset(cmds,0,sizeof(cmds));
    assert(apx_sd_init()==0);
    check_width(wide_mode==0&&APX_SD_BUS_WIDTH_MAX>=4);
    assert((registers[0x8040]&6)==0&&(registers[0x8000]&4)==0);
    assert(registers[0x4800]==0xffffffff&&!(registers[0x9884]&0x40));
    assert(apx_sd_get_info()->sectors==(hc?67108864:3862528));
    /* identification at the slowest divider the 8-bit register has; then 24 MHz, or 48 MHz in high speed */
    assert(id_divider_seen==255);
    assert(apx_sd_get_info()->errors==0);
    if(hs_mode==1) {
        assert(cmd6_count==2&&cmd6_args[0]==0x00ffff01u&&cmd6_args[1]==0x80ffff01u&&hs_switched);
        assert(registers[0x9888]==2&&apx_sd_get_info()->clock_hz==48000000&&apx_sd_get_info()->high_speed);
        assert((registers[0x9880]&0x80000008u)==0x80000008u);
        assert(apx_sd_set_clock(49000000)==APX_SD_RANGE&&!apx_sd_set_clock(25000000)&&apx_sd_get_info()->clock_hz==24000000);
        assert(!apx_sd_set_clock(48000000)&&registers[0x9888]==2);
    } else {
        assert(cmd6_count==(hs_mode==0?0:1)&&!hs_switched); /* class 10 clear: never tried; else a check only */
        assert(registers[0x9888]==5&&apx_sd_get_info()->clock_hz==24000000&&!apx_sd_get_info()->high_speed);
        assert(!(registers[0x9880]&0x80000008u));
        assert(apx_sd_set_clock(100000)==APX_SD_RANGE&&apx_sd_set_clock(25000000)==APX_SD_RANGE&&apx_sd_set_clock(500000)==APX_SD_RANGE);
    }
    assert(!apx_sd_set_clock(12000000)&&apx_sd_get_info()->clock_hz==12000000&&registers[0x9888]==11);
    assert(!apx_sd_set_clock(24000000)&&apx_sd_get_info()->clock_hz==24000000&&registers[0x9888]==5);
    assert(!apx_sd_set_clock(562500)&&registers[0x9888]==255&&apx_sd_get_info()->clock_hz==562500);
    assert(!apx_sd_set_clock(24000000));
    memset(cmds,0,sizeof(cmds));
}
/* Four-bit bus: it is negotiated, verified by reading the SD status on both widths, and every way it can go wrong ends with a working card on one line. */
static void wide_tests(uint8_t *ram) {
#if APX_SD_BUS_WIDTH_MAX<4
    (void)ram;wide_mode=0; /* this board's lines are not all wired to the SD socket pads the driver could use: it must stay on one line and never send ACMD6 */
    initialize(0);assert(apx_sd_get_info()->bus_width==1&&acmd6_count==0&&acmd13_count==0&&apx_sd_get_info()->bus_probe==APX_SD_WIDE_NOT_TRIED);check_pads(0);
    return;
#endif
    for(unsigned hc=0;hc<2;++hc) {
        wide_mode=0;dat_stuck_lines=0;initialize(hc);
        const struct apx_sd_info *info=apx_sd_get_info();
        assert(acmd6_count==1&&acmd13_count==2&&info->bus_width==4&&info->bus_probe==APX_SD_WIDE_OK&&info->bus_probe_detail==0);
        /* data of every kind flows in four-bit mode (the model asserts the controller and card widths agree on every transfer) */
        assert(apx_sd_read(10,ram,8)==0);check_pattern(ram,10,8);
        fill_pattern(ram,4000,4);assert(apx_sd_write(4000,ram,4)==0&&apx_sd_sync()==0);check_written(ram,4000,4);
        /* a card error in the middle re-initialises and negotiates again (once more, not blocked) */
        read_crc_once=1;assert(apx_sd_read(50,ram,1)==0);
        assert(apx_sd_get_info()->retries==1&&apx_sd_get_info()->bus_width==4&&acmd6_count==2);
        /* shutdown gives the pads back and leaves the controller one bit wide */
        apx_sd_shutdown();check_pads(0);assert(!(registers[0x9880]&4u));
        assert(apx_sd_init()==0&&apx_sd_get_info()->bus_width==4);check_pads(1);
        /* every failure: one line, the pads restored, the card usable, the reason reported */
        static const struct { unsigned mode,lines,code;uint32_t detail; } cases[]={
            {1,0,APX_SD_WIDE_REFUSED,0xfe},        /* ACMD6 answered with an error: APX_SD_CARD_ERROR = -2, low byte 0xfe */
            {2,1u<<1,APX_SD_WIDE_MISMATCH,2},      /* DAT1 delivers nothing: the first byte that differs is byte 1 (reported + 1) */
            {2,1u<<2,APX_SD_WIDE_MISMATCH,2},
            {2,1u<<3,APX_SD_WIDE_MISMATCH,1},      /* DAT3 carries bit 7 of byte 0: the DAT_BUS_WIDTH field itself reads wrong */
            {3,0,APX_SD_WIDE_READ,0xfe},           /* controller CRC error on the four-bit status read */
            {4,0,APX_SD_WIDE_READ,0xff},           /* it never arrives: APX_SD_TIMEOUT = -1 */
            {5,0,APX_SD_WIDE_NARROW_READ,0xff},    /* the one-bit reference read already fails */
            {6,0,APX_SD_WIDE_MISMATCH,1}};         /* the card still says DAT_BUS_WIDTH 00 */
        for(unsigned c=0;c<sizeof(cases)/sizeof(cases[0]);++c) {
            wide_mode=cases[c].mode;dat_stuck_lines=cases[c].lines;
            initialize(hc);info=apx_sd_get_info();
            assert(info->bus_width==1&&info->bus_probe==cases[c].code&&!bus4);
            assert(info->errors==0&&info->retries==0);
            assert((info->bus_probe_detail&0xff)==cases[c].detail);
            if(cases[c].mode==2&&cases[c].detail==2) { /* byte 1 as the card sent it and as the controller saw it with the line dead */
                const unsigned line=cases[c].lines==2?1:2;
                const uint8_t pattern=(uint8_t)((1*37+11)|0x6e);
                assert(((info->bus_probe_detail>>16)&0xff)==pattern&&((info->bus_probe_detail>>8)&0xff)==(pattern&(uint8_t)~((1u<<line)|(1u<<(line+4)))));
            }
            /* the card works on one line, all kinds of transfers (the model rejects any data command with the widths disagreeing) */
            assert(apx_sd_read(10,ram,8)==0);check_pattern(ram,10,8);
            fill_pattern(ram,4100,3);assert(apx_sd_write(4100,ram,3)==0&&apx_sd_sync()==0);check_written(ram,4100,3);
            /* blocked for this card: another init tries nothing, a different card is tried again */
            const unsigned tries=acmd6_count;
            assert(apx_sd_init()==0&&apx_sd_get_info()->bus_width==1&&acmd6_count==tries&&apx_sd_get_info()->bus_probe==cases[c].code);
            check_pads(0);
            /* a read error re-initialises: still no new attempt on this card */
            read_crc_once=1;assert(apx_sd_read(60,ram,1)==0&&acmd6_count==tries&&apx_sd_get_info()->bus_width==1);
            wide_mode=0;dat_stuck_lines=0;++cid_generation; /* another card, which works */
            assert(apx_sd_init()==0&&apx_sd_get_info()->bus_width==4&&apx_sd_get_info()->bus_probe==APX_SD_WIDE_OK);check_pads(1);
            assert(apx_sd_read(70,ram,2)==0);check_pattern(ram,70,2);
        }
    }
    /* the restart after a failed four-bit attempt waits and retries while the card is not ready yet */
    for(unsigned hc=0;hc<2;++hc) {
        wide_mode=0;dat_stuck_lines=0;wide_busy_ticks=0;initialize(hc);
        wide_mode=3;wide_busy_ticks=90000; /* 2.7 s deaf: the early attempts find the card deaf, only the later, longer waits get through */
        assert(apx_sd_init()==0);
        assert(apx_sd_get_info()->bus_width==1&&apx_sd_get_info()->bus_probe==APX_SD_WIDE_READ&&apx_sd_get_info()->initialized);
        assert(apx_sd_read(10,ram,2)==0);check_pattern(ram,10,2);
        wide_mode=0;wide_busy_ticks=0;cid_generation=1;assert(apx_sd_init()==0); /* another card clears the per-card block that the failure above set */
        /* a card that never comes back in time: init fails (timeout) instead of hanging, and a later init works */
        initialize(hc);wide_mode=3;wide_busy_ticks=1000000;
        assert(apx_sd_init()==APX_SD_TIMEOUT&&!apx_sd_get_info()->initialized);
        wide_busy_ticks=0;++cid_generation;wide_mode=0;
        assert(apx_sd_init()==0&&apx_sd_get_info()->bus_width==4);
    }
    wide_mode=0;dat_stuck_lines=0;wide_busy_ticks=0;
}
int main(void) {
    uint8_t *ram=mmap((void *)0x021a0000,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(ram!=(void *)-1);
    for(unsigned hc=0;hc<2;++hc) {
        initialize(hc);memset(ram,0x5a,8192);
        assert(disk_status(0)==0&&disk_status(1)==STA_NOINIT);
        LBA_t sectors;assert(disk_ioctl(0,GET_SECTOR_COUNT,&sectors)==RES_OK&&sectors==apx_sd_get_info()->sectors);
        assert(apx_sd_read(123,ram+1,8)==0);
        for(unsigned s=0;s<8;++s)for(unsigned i=0;i<512;++i)assert(ram[1+s*512+i]==((123+s)*97+i)%256);
        assert(ram[0]==0x5a&&ram[4097]==0x5a&&polls);
        uint8_t original[4096];memcpy(original,ram+1,sizeof(original));
        assert(disk_write(0,ram+1,123,8)==RES_OK);assert(apx_sd_get_info()->writes==8);
        memset(ram+1,0,4096);assert(apx_sd_read(123,ram+1,8)==0);
        assert(!memcmp(ram+1,original,4096)&&ram[0]==0x5a&&ram[4097]==0x5a);
        assert(disk_ioctl(0,CTRL_SYNC,0)==RES_OK);
        assert(apx_sd_write(0,NULL,1)==APX_SD_RANGE);
        assert(apx_sd_write(0,ram,0)==APX_SD_RANGE);
        assert(apx_sd_write((uint32_t)apx_sd_get_info()->sectors-1,ram,2)==APX_SD_RANGE);
        assert(apx_sd_write(0,(void *)0x023fffff,1)==APX_SD_RANGE);
        assert(disk_write(1,ram,0,1)==RES_PARERR);
        assert(apx_sd_read((uint32_t)apx_sd_get_info()->sectors-1,ram,2)==APX_SD_RANGE);
        assert(apx_sd_read(0,ram,0)==APX_SD_RANGE);
        assert(apx_sd_read(0,(void *)0x023fffff,1)==APX_SD_RANGE);
        initialize(hc);read_crc_once=1;
        assert(!apx_sd_set_clock(12000000));assert(!apx_sd_read(0,ram,1));
        assert(apx_sd_get_info()->retries==1&&apx_sd_get_info()->errors==1&&apx_sd_get_info()->reads==1);
        assert(apx_sd_get_info()->clock_hz==12000000&&apx_sd_get_info()->last_failure_command==17);
        initialize(hc);write_crc_once=1;assert(!apx_sd_write(2,ram,1));
        assert(apx_sd_get_info()->retries==1&&apx_sd_get_info()->writes==1&&apx_sd_get_info()->errors==1);
        initialize(hc);read_crc_once=replace_on_error=1;assert(apx_sd_read(0,ram,1)==APX_SD_CARD_ERROR);
        assert(!apx_sd_get_info()->initialized&&apx_sd_get_info()->retries==1);
        initialize(hc);crc_error=1;assert(apx_sd_read(0,ram,1)==APX_SD_CARD_ERROR);assert(!apx_sd_get_info()->initialized);
        assert(apx_sd_get_info()->retries==2&&apx_sd_get_info()->errors==3);
        assert(disk_status(0)&STA_NOINIT);assert(disk_read(0,ram,0,1)==RES_NOTRDY);
        initialize(hc);data_stuck=1;assert(apx_sd_read(0,ram,1)==APX_SD_TIMEOUT);assert(!apx_sd_get_info()->initialized);
        initialize(hc);present=0;assert(apx_sd_read(0,ram,1)==APX_SD_TIMEOUT);
        present=1;assert(apx_sd_init()==0);assert(apx_sd_read(0,ram,1)==0);
        initialize(hc);write_crc_error=1;assert(apx_sd_write(2,ram,1)==APX_SD_CARD_ERROR);
        assert(!apx_sd_get_info()->initialized&&disk_write(0,ram,0,1)==RES_NOTRDY);
        initialize(hc);data_stuck=1;assert(apx_sd_write(2,ram,1)==APX_SD_TIMEOUT);
        initialize(hc);busy_stuck=1;assert(apx_sd_write(2,ram,1)==APX_SD_TIMEOUT);
        initialize(hc);status_stuck=1;assert(apx_sd_sync()==APX_SD_TIMEOUT);
        initialize(hc);r1_write_protected=1;assert(apx_sd_write(2,ram,1)==APX_SD_WRITE_PROTECTED);
        assert(apx_sd_get_info()->write_protected);
        initialize(hc);streaming_tests(ram);
        initialize(hc);write_stream_tests(ram);
        for(unsigned bit=12;bit<=13;++bit) {
            initialize(hc);set_bits(bit,bit,1);assert(apx_sd_init()==0);
            assert(disk_status(0)==STA_PROTECT&&disk_write(0,ram,0,1)==RES_WRPRT);
        }
    }
    /* high speed: only when the card says it can, only after both CMD6 passes, and any failure leaves it at default speed */
    for(unsigned hc=0;hc<2;++hc) {
        hs_mode=1;initialize(hc);assert(apx_sd_get_info()->high_speed);
        assert(apx_sd_read(10,ram,8)==0);check_pattern(ram,10,8);
        {   /* an error after the switch: the re-initialisation switches again and the old clock is restored */
            assert(!apx_sd_set_clock(36000000)&&registers[0x9888]==3);
            read_crc_once=1;assert(apx_sd_read(50,ram,1)==0);
            assert(apx_sd_get_info()->high_speed&&apx_sd_get_info()->clock_hz==36000000&&apx_sd_get_info()->retries==1);
        }
        hs_mode=2;initialize(hc);assert(!apx_sd_get_info()->high_speed&&apx_sd_read(10,ram,1)==0);
        hs_mode=3;initialize(hc);assert(!apx_sd_get_info()->high_speed&&apx_sd_get_info()->initialized&&apx_sd_read(10,ram,1)==0);
        hs_mode=0;initialize(hc);assert(!apx_sd_get_info()->high_speed);
    }
    hs_mode=0;
    wide_tests(ram);
    stuck=1;assert(apx_sd_init()==APX_SD_TIMEOUT);assert(!apx_sd_get_info()->initialized);
    initialize(0);set_bits(83,80,15);set_bits(73,62,4095);
    assert(apx_sd_init()==APX_SD_UNSUPPORTED);assert(!apx_sd_get_info()->initialized);
    initialize(1);set_bits(69,48,0x1ffff);assert(apx_sd_init()==0);
    assert(apx_sd_get_info()->sectors==134217728); /* 64 GiB SDXC */
    assert(apx_sd_read(134217727,ram,1)==0);
    set_bits(69,48,0x3fffff);assert(apx_sd_init()==0);
    assert(apx_sd_get_info()->sectors==UINT64_C(0x100000000));
    assert(apx_sd_read(UINT32_MAX,ram,1)==0); /* Last 32-bit command address */
    assert(apx_sd_write(UINT32_MAX,ram+1,1)==0); /* Last 32-bit write command address */
    puts("SD model: open-ended CMD18 streams (reuse, stop on write/sync/clock/error), CMD6 high speed (success, not selectable, CRC error), four-bit bus (ACMD6 + width-checked SD status, pad restore, per-card block, 8 failure modes; one line only where the board limits it), 8-bit divider, SDSC/SDHC RX/TX, DMA bounds, CRC/busy/status timeouts, protection, bounded read/write retry, clock/stat preservation and changed-CID refusal passed");
}
