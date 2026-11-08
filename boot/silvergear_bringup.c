/* Returning SRAM-only power/DDR diagnostic. No SPI driver or vendor calls.
 * Stock PM evidence: 0202b2cc and 0202b54c. DDR setup is a separate SRAM stage.
 * No host-supplied SPR or PM address is ever accepted for writing. */
#include <stdint.h>
#ifdef BRINGUP_HOST
uint32_t bringup_rd(unsigned r),bringup_load(uint32_t a);
void bringup_wr(unsigned r,uint32_t v),bringup_store(uint32_t a,uint32_t v);
#define rd bringup_rd
#define wr bringup_wr
#define load bringup_load
#define store bringup_store
#else
static inline uint32_t rd(unsigned r) {
    uint32_t v;__asm__ volatile("l.mfspr %0,%1,0":"=&r"(v):"r"(r):"memory");return v;
}
static inline void wr(unsigned r,uint32_t v) {
    __asm__ volatile("l.mtspr %0,%1,0"::"r"(r),"r"(v):"memory");
}
static uint32_t load(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static void store(uint32_t a,uint32_t v) { *(volatile uint32_t *)(uintptr_t)a=v; }
#endif
#ifndef BRINGUP_BUILD_ID
#define BRINGUP_BUILD_ID 0u
#endif
#define MAGIC 0x50585041u
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
static const uint32_t snapshots[]={
    0x880c,0x8858,0x885c,0x8860,0x8864,0x8868,
    0x8840,0x8844,0x8848,0x884c,0x8850,0x1800,0x2000,0x88e0,
    0x9000,0x9244,0x924c,0xf800,0xf804,0xf810,0xf818,0xf81c,
    0xf840,0xf844,0xf848,0xf860,0xf88c,0xf894,0xf8a4,0xf8ac
};
struct request {
    uint32_t magic,sequence,command,status,version,build_id,address,length;
    uint32_t seed,mode,bad_address,expected,actual,spr_count,ddr_unlocked,reserved;
    uint32_t spr[32];
    uint8_t pm[14];
};
_Static_assert(sizeof(struct request)==208,"bringup ABI changed");
static void upd(unsigned r,uint32_t mask,uint32_t bits) { wr(r,(rd(r)&~mask)|(bits&mask)); }
static int pm_byte(unsigned value,uint8_t *out) {
    wr(0x88ec,value);
    for(unsigned n=0;n<20000;++n)if(!(rd(0x88e0)&8)) {
        if(out)*out=rd(0x88ec)&255;
        return 0;
    }
    return 2;
}
static int transfer(unsigned address,unsigned value,uint8_t *out) {
    if(rd(0x88e0)&0x108)return 3;
    uint32_t saved=rd(0x88e0)&0x140;
    upd(0x88e0,0x140,0x140);
    int e=pm_byte(address,0);if(!e)e=pm_byte(value,out);
    upd(0x88e0,0x140,saved);return e;
}
static int pm_update(struct request *p,unsigned address,unsigned mask,unsigned bits) {
    uint8_t value;int e=transfer(address,0,&value);
    if(!e)e=transfer(address+1,(value&~mask)|(bits&mask),0);
    if(!e)e=transfer(address,0,&value);
    if(!e&&((value^bits)&mask)) {
        p->bad_address=address;p->expected=bits&mask;p->actual=value;return 4;
    }
    return e;
}
static int snapshot(struct request *p) {
    p->spr_count=COUNT(snapshots);
    /* DDR read ports can stall while their domain/controller is uninitialized.
     * Do not sample them during a power-only handshake. */
    for(unsigned i=0;i<COUNT(snapshots);++i) {
        unsigned r=snapshots[i];
        int known_ddr_read=r==0xf818||r==0xf81c||r==0xf860||r==0xf8ac;
        p->spr[i]=(i<17||(p->ddr_unlocked&&known_ddr_read))?rd(r):0;
    }
    for(unsigned i=0;i<14;++i) {
        int e=transfer(0x40+2*i,0,p->pm+i);if(e)return e;
    }
    return 0;
}
static int rails(struct request *p) {
    /* Stock 0202b54c(1): PM44 is read then written unchanged. It must not
     * be replaced by the older board's literal zero. Preserve PM5a too. */
    uint8_t value;int e=transfer(0x40,0,&value);
    if(e)return e;
    if(!(value&4))return 3;
    /* PM42 does not echo the stock 0xe0 write. PM44 is read and rewritten
     * unchanged. Verify the writable hold fields separately. */
    if((e=transfer(0x43,0xe0,0))||(e=transfer(0x44,0,&value))||
       (e=transfer(0x45,value,0))||(e=pm_update(p,0x58,8,8))||
       (e=pm_update(p,0x40,0x10,0x10)))return e;
    return pm_update(p,0x5a,8,8);
}
static uint32_t pattern(uint32_t address,unsigned mode,uint32_t seed) {
    return mode==0?seed:address^seed;
}
void rescue(struct request *p) {
    if(p->magic!=MAGIC)return;
    p->status=1;p->version=1;p->build_id=BRINGUP_BUILD_ID;
    /* Keep the diagnostic loader recoverable between host checks and code
     * updates. The ROM can rearm this in its USB ISR, so disable on each call.
     * Command 7 explicitly restores the saved policy at successful completion. */
    if(!p->reserved)p->reserved=rd(0x880c);
    wr(0x880c,0x5500);
    int e=0;unsigned cmd=p->command;
    if(cmd==0)e=snapshot(p);
    else if(cmd==1) {
        /* Narrow power hold first, without enabling DDR/peripheral rails. */
        e=pm_update(p,0x40,4,4);if(!e)e=pm_update(p,0x5a,8,8);
        if(!e)e=snapshot(p);
    } else if(cmd==2) { e=rails(p);if(!e)e=snapshot(p); }
    else if(cmd==6) {
        if(p->seed!=0x44445231u)e=3; /* explicit host confirmation of recovered calibration stage */
        else {
            unsigned ready=rd(0xf860)&15;
            /* Stock polls f860; do not assume the f800 start control echoes. */
            if((rd(0x1800)&1)||(rd(0x2000)&1)||(ready!=3&&ready!=7&&ready!=15)) {
                p->bad_address=0xf860;p->expected=3;p->actual=ready;e=3;
            }
            else { p->ddr_unlocked=1;e=snapshot(p); }
        }
    } else if(cmd==7) {
        if(p->seed&~0x1fu)e=3;
        else wr(0x880c,(p->seed&0x10)?0x50000u|(p->seed&7):0x5500u);
    } else if(cmd==3||cmd==4) {
        uint32_t a=p->address,n=p->length;
        if(!p->ddr_unlocked||(rd(0x1800)&1)||(a&3)||(n&3)||!n||n>4096||
           a<0x02000000||a>=0x02400000||n>0x02400000-a||p->mode>1)e=3;
        else for(uint32_t offset=0;offset<n;offset+=4) {
            uint32_t address=a+offset,value=pattern(address,p->mode,p->seed);
            if(cmd==3)store(address,value);
            else {
                uint32_t actual=load(address);
                if(actual!=value) {
                    p->bad_address=address;p->expected=value;p->actual=actual;e=4;break;
                }
            }
        }
    } else e=3;
    p->status=e;
}
