#include <appotech/clock.h>
#include <appotech/soc.h>
#include "board.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t regs[65536];
static unsigned raised,lowered,bad_write;
/* The 32 kHz-domain timer control: a write is visible after `latency` ticks, and the first `drop_writes` are lost. */
static uint32_t ticks,write_at,pending,latency,drop_writes;
static int pending_valid;
uint32_t apx_spr_read(uint32_t r) {
 if(r==0x9804){ticks+=3;return ticks&0x3fff;}
 if(r==0x9800&&pending_valid&&ticks-write_at>=latency){regs[0x9800]=pending;pending_valid=0;}
 return regs[r];
}
void apx_spr_write(uint32_t r,uint32_t v){
 if(r==0x9800&&(latency||drop_writes)){
  if(drop_writes){--drop_writes;return;}
  pending=v;write_at=ticks;pending_valid=1;return;
 }
 regs[r]=v;
}
int watchdog_feeds;
int apx_watchdog_feed(void){return ++watchdog_feeds;}
void apx_clock_apply_144(void) {++raised;regs[0x8858]=bad_write?0:0x1123;regs[0x885c]=0x112a8800;}
void apx_clock_apply_96(void) {++lowered;regs[0x8858]=0x1623;regs[0x885c]=0x0b71b000;}
uint32_t apx_clock_measure(void) {return regs[0x8858]==0x1123?1593:2389;}
static void baseline(void) {regs[0x8858]=0x1623;regs[0x885c]=0x0b71b000;regs[0x9800]=0x721;regs[0x9808]=0x3fff;}
int main(int argc,char **argv) {
 baseline();
 if(argc>1&&!strcmp(argv[1],"spi")){
  /* Cold boot: the bootstrap's 144 MHz pair is accepted as is, with no PLL transition. */
  regs[0x8858]=0x1123;regs[0x885c]=0x112a8800;regs[0x9800]=0x701;
  assert(!apx_clock_prepare_spi());assert(!lowered&&!raised&&regs[0x9800]==0x721);
  assert(!apx_clock_init());struct apx_clock_info i;apx_clock_get_info(&i);
  assert(!i.error&&i.baseline_loop_ticks==0&&i.cpu_hz_estimate==APX_CPU_HZ_ESTIMATE);
  assert(raised==0&&lowered==(APX_CPU_MHZ==96)); /* only the downclocked build moves the PLL */
  assert(i.active_loop_ticks==(APX_CPU_MHZ==144?1593u:2389u));
  assert(!apx_clock_init()&&lowered==(APX_CPU_MHZ==96)); /* idempotent */
  assert(!apx_clock_restore()&&regs[0x8858]==0x1623); /* BootROM handoff still lands on the loader pair */
  /* Guards: unknown pair, unknown timer or period are rejected before anything changes. */
  /* Slow and lossy timer-control writes: verified and retried, but bounded. */
  latency=39;regs[0x9800]=0x701;assert(!apx_clock_prepare_spi()&&regs[0x9800]==0x721);
  latency=0;pending_valid=0;regs[0x9800]=0x701;drop_writes=3;assert(!apx_clock_prepare_spi()&&regs[0x9800]==0x721&&!drop_writes&&apx_clock_prepare_attempts()==4);
  regs[0x9800]=0x701;drop_writes=2000;assert(apx_clock_prepare_spi()==4&&drop_writes==2000-768&&apx_clock_prepare_attempts()==769);drop_writes=0;
  regs[0x9800]=0x701;latency=39;drop_writes=2;assert(!apx_clock_prepare_spi());
  latency=0;pending_valid=0;regs[0x9800]=0x701;
  regs[0x8858]=0x1723;assert(apx_clock_prepare_spi()==1);
  regs[0x8858]=0x1123;regs[0x885c]=0x112a8800;regs[0x9800]=0x555;assert(apx_clock_prepare_spi()==2);
  regs[0x9800]=0x701;regs[0x9808]=0x2bf1;assert(apx_clock_prepare_spi()==3);
  return 0;
 }
 if(argc>1&&!strcmp(argv[1],"spi-96")){ /* BootROM-launch rehearsal pair still works */
  regs[0x9800]=0x701;assert(!apx_clock_prepare_spi()&&regs[0x9800]==0x721&&!lowered);
  assert(!apx_clock_init());struct apx_clock_info i;apx_clock_get_info(&i);assert(i.baseline_loop_ticks==2389);
  return 0;
 }
 if(argc>1){
  if(!strcmp(argv[1],"bad-pair"))regs[0x8858]=0x1723;
  if(!strcmp(argv[1],"bad-timer"))regs[0x9808]=0x2bf1;
  if(!strcmp(argv[1],"bad-write"))bad_write=1;
  assert(apx_clock_init());assert(!lowered);
  if(bad_write)assert(raised==(APX_CPU_MHZ==144));else assert(!raised);
  return 0;
 }
 assert(!apx_clock_init());struct apx_clock_info i;apx_clock_get_info(&i);
 assert(i.cpu_hz_estimate==APX_CPU_HZ_ESTIMATE&&i.requested_mhz==APX_CPU_MHZ&&!i.error);
 assert(i.baseline_loop_ticks==2389&&i.active_loop_ticks==(APX_CPU_MHZ==144?1593u:2389u));
 assert(raised==(APX_CPU_MHZ==144));assert(!apx_clock_init());assert(raised==(APX_CPU_MHZ==144));
 /* Unknown live pairs are never overwritten during shutdown. */
 regs[0x8858]=0x1723;assert(apx_clock_restore()==1&&!lowered);
 regs[0x8858]=APX_CPU_MHZ==144?0x1123:0x1623;assert(!apx_clock_restore());
 assert(lowered==(APX_CPU_MHZ==144)&&regs[0x8858]==0x1623&&regs[0x885c]==0x0b71b000);
 apx_clock_get_info(&i);assert(i.cpu_hz_estimate==96000000&&!i.error);
 puts("Clock model: fixed profiles, incoming pair/timer guards, readback, idempotent init and verified rollback passed");
}
