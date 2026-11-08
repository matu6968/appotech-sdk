#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BRINGUP_HOST
#include "../boot/silvergear_bringup.c"
static uint32_t registers[65536],memory[0x100000];
static uint8_t pm[14];
static unsigned phase,address,busy,alias,writes,ddr_ready,last44;
uint32_t bringup_rd(unsigned r) {
    assert(r<65536);
    if(r>=0xf800&&r<=0xf8ff)assert(ddr_ready); /* no pre-power DDR reads */
    return registers[r]|((r==0x88e0&&busy)?8:0);
}
void bringup_wr(unsigned r,uint32_t v) {
    if(r==0x880c){assert(v==0x5001d||v==0x50005||v==0x5500);registers[r]=v==0x5500?0:0x1d;return;}
    if(r==0x88e0) {
        assert(!((v^registers[r])&~0x148u));
        if(!(registers[r]&0x100)&&(v&0x100))phase=0;
        registers[r]=v&~8u;return;
    }
    assert(r==0x88ec); /* no SPI, PLL, DMA, camera or printer writes */
    assert(registers[0x88e0]&0x100);
    if(!phase){address=v;phase=1;return;}
    assert(address>=0x40&&address<=0x5b);
    if(address&1) {
        assert(address==0x41||address==0x43||address==0x45||address==0x59||address==0x5b);
        if(address==0x43)assert(v==0xe0); /* status/ack byte need not echo */
        else if(address==0x45){last44=v;pm[2]=v;}
        else pm[(address-0x40)/2]=v;
    } else registers[r]=pm[(address-0x40)/2];
}
uint32_t bringup_load(uint32_t a) {
    assert(a>=0x02000000&&a<0x02400000&&!(a&3));
    return memory[((a-0x02000000)&(alias?0x1fffff:0x3fffff))/4];
}
void bringup_store(uint32_t a,uint32_t v) {
    assert(a>=0x02000000&&a<0x02400000&&!(a&3));++writes;
    memory[((a-0x02000000)&(alias?0x1fffff:0x3fffff))/4]=v;
}
void bringup_model_reset(void) {
    memset(registers,0,sizeof(registers));memset(memory,0,sizeof(memory));
    for(unsigned i=0;i<14;++i)pm[i]=i*13+7;
    pm[0]=0x82;pm[2]=0x5a;pm[13]=0x62;
    registers[0x88e0]=0x220003;registers[0x880c]=0x1d;
    phase=address=busy=alias=writes=ddr_ready=last44=0;
}
void bringup_model_ready(void) { ddr_ready=1;registers[0xf800]=0;registers[0xf860]=7; }
void bringup_model_corrupt(void) { memory[0x1234/4]^=1; }
unsigned bringup_model_pm(unsigned i) { assert(i<14);return pm[i]; }
unsigned bringup_model_last44(void) { return last44; }
void *bringup_model_memory(void) { return memory; }
void bringup_model_alias(unsigned value) { alias=value; }
#ifndef BRINGUP_LIBRARY
static unsigned call(struct request *p,unsigned command) {
    p->command=command;p->status=99;++p->sequence;rescue(p);return p->status;
}
static void pass(struct request *p,unsigned cmd) {
    for(unsigned a=0x02000000;a<0x02400000;a+=4096) {
        p->address=a;p->length=4096;assert(!call(p,cmd));
    }
}
int main(void) {
    struct request p={.magic=MAGIC};bringup_model_reset();
    assert(!call(&p,0)&&p.version==1&&p.pm[0]==0x82&&p.spr_count==30);
    assert(call(&p,2)==3); /* rails require a latched domain */
    assert(!call(&p,1)&&pm[0]==0x86&&pm[13]==0x6a);
    assert(!call(&p,2)&&pm[0]==0x96&&last44==0x5a&&pm[2]==0x5a);
    assert((pm[12]&8)&&pm[13]==0x6a&&registers[0x88e0]==0x220003);
    assert(call(&p,6)==3);bringup_model_ready();p.seed=0x44445231;
    assert(!call(&p,6)&&p.ddr_unlocked);
    assert(p.spr[25]==7&&p.reserved==0x1d&&registers[0x880c]==0);
    for(unsigned i=0;i<4;++i) {
        p.mode=i>=2;p.seed=i==0?0:i==1?0xffffffff:i==2?0xa5a5a5a5:0x5a5a5a5a;
        pass(&p,3);pass(&p,4);
    }
    bringup_model_corrupt();p.address=0x02001000;p.length=4096;
    assert(call(&p,4)==4&&p.bad_address==0x02001234&&(p.actual^p.expected)==1);
    unsigned before=writes;
    p.address=0x023ffffc;p.length=8;assert(call(&p,3)==3&&writes==before);
    p.address=0xffffffff;p.length=4;assert(call(&p,3)==3&&writes==before);
    p.address=0x02000000;p.length=4097;assert(call(&p,3)==3&&writes==before);
    p.length=4096;registers[0x1800]=1;assert(call(&p,3)==3&&writes==before);
    registers[0x1800]=0;alias=1;p.mode=1;p.seed=0xa5a5a5a5;pass(&p,3);
    p.address=0x02000000;assert(call(&p,4)==4&&p.bad_address==0x02000000);
    p.seed=0x1d;assert(!call(&p,7)&&registers[0x880c]==0x1d);
    p.seed=0x100;assert(call(&p,7)==3&&registers[0x880c]==0);
    busy=1;assert(call(&p,1)==3&&!(registers[0x88e0]&0x100));busy=0;
    registers[0x88e0]|=0x100;assert(call(&p,1)==3);
    p.magic=0;assert(call(&p,1)==99);
    puts("Bringup model: PM masks/rail sequence, readback, busy refusal, DDR unlock/bounds, four full-RAM passes, data and address-alias faults passed");
}
#endif
