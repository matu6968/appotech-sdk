#include <appotech/printer.h>
#include "board.h"
#include <appotech/camera.h>
#include <appotech/battery.h>
#include <appotech/gpio.h>
#include <appotech/soc.h>
#include <appotech/adc.h>
#include <appotech/debug.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/printer_image.h"
static uint32_t regs[65536],ticks,pins[5],pulses,clocks;
static unsigned timer_start,timer_frequency=1000000;
static uint8_t serial[384],latched[384];
static unsigned latched_valid,latches,shifted_while_hot,drop_at_pulse;
static const uint8_t *expected;
static unsigned expected_w,expected_stride;
static unsigned expected_luma,exposures[384],stall_at_pulse;
static unsigned battery_deferrals;
static unsigned camera_state,adc_value=800,adc_pending,adc_fail,timer_stuck,head,paper_removed,battery_valid=1,battery_mv=4200;
static uint32_t levels(unsigned g) {
 uint32_t gpio=regs[0x8380+4*g]&~regs[0x8100+4*g]&~regs[0x8140+4*g];
 return (((regs[0x8000+4*g]&~regs[0x8040+4*g])|regs[0x81c0+4*g])&~regs[0x8200+4*g])&gpio;
}
static void observe(void) {
 uint32_t next[5];for(unsigned g=0;g<5;++g)next[g]=levels(g);
 if(!(pins[3]&0x2000)&&(next[3]&0x2000)) {
  serial[clocks%384]=!!(next[3]&0x8000);++clocks;
  if(head)++shifted_while_hot;
 }
 if((pins[1]&0x20)&&!(next[1]&0x20)) {
  assert(!head);
  for(unsigned x=0;x<384;++x)latched[x]=serial[(clocks+x)%384];
  latched_valid=clocks>=384;++latches;
 }
 if(!(pins[0]&0x200)&&(next[0]&0x200)) {
  assert((regs[0x8868]&0x1fff)==0x182e);
  struct apx_printer_info i;apx_printer_get_info(&i);
  assert(expected&&latched_valid&&(next[1]&0x40)&&(next[1]&0x20));
  assert(!paper_removed&&i.steps);
  unsigned half=(i.steps-1)&1,left=(384-expected_w)/2,dots=0;
  for(unsigned x=0;x<384;++x) {
   unsigned bit=0;
   if(x>=left&&x-left<expected_w&&((x&1)==(half?0u:1u))){
    if(expected_luma){unsigned threshold=i.plane?256-i.plane*8:255;
     bit=expected[i.row*expected_stride+x-left]<threshold;
    }else bit=(expected[i.row*expected_stride+(x-left)/8]>>(7-((x-left)&7)))&1;
   }
   assert(latched[x]==bit);dots+=bit;
   if(bit)++exposures[x];
  }
  assert(dots);++pulses;
  if(drop_at_pulse==pulses)paper_removed=1;
  if(stall_at_pulse==pulses)timer_stuck=1;
 }
 for(unsigned g=0;g<5;++g)pins[g]=next[g];
 head=!!(next[0]&0x200);
}
uint32_t apx_ticks(void){return ticks++;}
void apx_maintenance(void){}
uint32_t apx_spr_read(uint32_t r){if(r==0x9804)return apx_ticks()&0x3fff;if(r==0x9834&&!timer_stuck&&(regs[0x9830]&1))return (regs[r]+(uint64_t)(ticks-timer_start)*timer_frequency/32768)&0xffff;if(r==0x8000)return (regs[r]&~0x80u)|(paper_removed?0x80u:0);return regs[r];}
void apx_spr_write(uint32_t r,uint32_t v){regs[r]=v;if(r==0x9834)timer_start=ticks;if(r>=0x8000&&r<=0x8390)observe();}
/* Host substitutes model only the six native leaves' observable SPR effects.
 * Their actual instruction bytes are checked separately against stock. */
static void native_update(unsigned r,unsigned mask,unsigned v){apx_spr_write(r,(apx_spr_read(r)&~mask)|v);}
void apx_printer_reference_strobe_high(void){native_update(0x8000,0x200,0x200);}
void apx_printer_reference_strobe_low(void){native_update(0x8000,0x200,0);}
void apx_printer_reference_latch_high(void){native_update(0x8004,0x20,0x20);}
void apx_printer_reference_latch_low(void){native_update(0x8004,0x20,0);}
void apx_battery_get_info(struct apx_battery_info *out){memset(out,0,sizeof(*out));out->valid=battery_valid;out->millivolts=battery_mv;}
void apx_battery_defer_conversion(void){++battery_deferrals;}
void apx_camera_get_info(struct apx_camera_info *out){memset(out,0,sizeof(*out));out->state=camera_state;}
static int submit(const uint8_t *data,size_t capacity,unsigned w,unsigned h,unsigned stride) {
 int error=apx_printer_submit(data,capacity,w,h,stride);
 if(!error){expected=data;expected_w=w;expected_stride=stride;expected_luma=0;}
 return error;
}
static int submit_photo(const uint8_t *data,size_t capacity,unsigned w,unsigned h,unsigned stride) {
 int error=apx_printer_submit_photo(data,capacity,w,h,stride);
 if(!error){expected=data;expected_w=w;expected_stride=stride;expected_luma=0;}
 return error;
}
int apx_adc_acquire(unsigned owner){assert(owner==APX_ADC_PRINTER);return 0;}
int apx_adc_begin(unsigned owner,unsigned channel){assert(owner==APX_ADC_PRINTER&&channel<=11);adc_pending=1;return 0;}
int apx_adc_result(unsigned owner,uint16_t *value){(void)owner;if(!adc_pending||adc_fail)return -1;adc_pending=0;*value=adc_value;return 1;}
void apx_adc_release(unsigned owner){(void)owner;adc_pending=0;}
static struct apx_printer_info state(void){struct apx_printer_info i;apx_printer_get_info(&i);return i;}
static void inactive(void){
 struct apx_printer_info i=state();
 assert(!head&&!i.head_on&&!i.motor_on&&!i.power_on&&!(pins[1]&0x40));
 assert(!(pins[0]&8)&&!(pins[3]&1)&&!(pins[4]&0xc00));
 assert((regs[0x8868]&0x1fff)==0x190);
}
static void finish(void){for(unsigned i=0;i<200000&&apx_printer_busy();++i){apx_printer_poll();assert(!head);}assert(!apx_printer_busy());inactive();}
static void first_pulse(void){
 unsigned start=pulses;
 for(unsigned i=0;i<200000&&pulses==start&&apx_printer_busy();++i){apx_printer_poll();assert(!head);}
 assert(pulses>start&&apx_printer_busy());
}
static void put32(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;++n)p[n]=v>>(8*n);}
static unsigned command(unsigned cmd,const void *in,size_t n){uint8_t out[4096];size_t size;return apx_printer_command(cmd,in,n,out,&size);}
int main(void){
 regs[0x8340]=0xa55a00c0;
 regs[0x8868]=0x55aa8190;
 uint8_t src[8]={0,255,0,255,0,255,0,255},out[4]={0},bitmap[48]={0};int16_t errors[772];
 assert(!apx_printer_dither(src,8,1,8,out,1,8,1,errors,772));assert(out[0]==0xaa);
 assert(apx_printer_dither(src,8,1,7,out,1,8,1,errors,772)==APX_PRINTER_INVALID);
 assert(apx_printer_dither(src,8,1,8,out,1,9,1,errors,772)==APX_PRINTER_INVALID);
 /* Asymmetric 3x2 photo with padded source rows: clockwise is [white,black],
  * [black,white], [white,white]. Padding must not become image content. */
 const uint8_t photo[]={0,255,255,123,255,0,255,45};
 uint8_t rotated[8];memset(rotated,0xa5,sizeof(rotated));
 assert(!apx_printer_dither_clockwise(photo,3,2,4,rotated+1,3,2,3,errors,772));
 assert(rotated[0]==0xa5&&rotated[1]==0x40&&rotated[2]==0x80&&rotated[3]==0&&rotated[4]==0xa5);
 assert(!apx_printer_dither_clockwise(photo,3,2,4,rotated+1,6,4,6,errors,772));
 const uint8_t enlarged[]={0x30,0x30,0xc0,0xc0,0,0};
 assert(!memcmp(rotated+1,enlarged,sizeof(enlarged))&&rotated[0]==0xa5&&rotated[7]==0xa5);
 assert(apx_printer_dither_clockwise(photo,3,2,4,rotated,5,4,6,errors,772)==APX_PRINTER_INVALID);
 camera_state=APX_CAMERA_READY;assert(apx_printer_feed(2)==APX_PRINTER_BUSY);camera_state=APX_CAMERA_OFF;
 assert(submit(bitmap,1,384,1,48)==APX_PRINTER_INVALID);
 /* Paper sensing is independent of feed/heating and reports a sampled ADC
  * even with an empty mechanism. No-paper feed must not energize the motor. */
 paper_removed=1;assert(!apx_printer_check());finish();
 assert(state().error==APX_PRINTER_PAPER&&state().paper_valid&&state().temperature_valid);
 assert(state().steps==0&&pulses==0&&clocks==0);
 assert(!apx_printer_feed(3));finish();assert(state().error==APX_PRINTER_PAPER&&state().steps==0&&pulses==0);
 /* PA6 is the thermistor input; its internal pull-up is the divider bias (hardware test 18). */
 paper_removed=0;assert(!apx_printer_check());
 assert((regs[0x8868]&0x1fff)==0x182e);
 /* Another owner may alter the unowned battery-reference field. */
 regs[0x8868]^=0x8000;
 assert((regs[0x8040]&0x40)&&(regs[0x8080]&0x40)&&!(regs[0x80c0]&0x40));
 finish();
 assert(regs[0x8868]==0x55aa0190);
 assert(state().state==APX_PRINTER_DONE&&state().paper_valid&&state().temperature_valid&&state().steps==0&&pulses==0);
 assert(regs[0x8340]==0xa55a00c0);
 assert(!apx_printer_check());assert(regs[0x8340]==0xa55a0000&&(pins[1]&0x20));finish();
 memset(bitmap,0xaa,sizeof(bitmap));assert(!submit(bitmap,sizeof(bitmap),384,1,48));finish();
 assert(state().state==APX_PRINTER_DONE&&state().row==1&&state().steps==2&&pulses==32);
 assert(clocks==384*2&&!shifted_while_hot);
 /* Energy scale: pulses scale linearly, stay inside the 1112-count bound, idle-only. */
 assert(apx_printer_set_energy(99)==APX_PRINTER_INVALID&&apx_printer_set_energy(401)==APX_PRINTER_INVALID);
 unsigned base_max=state().max_heat_count,base_energy=state().energy_percent;assert(base_max&&!apx_printer_set_energy(200));
 assert(!submit(bitmap,sizeof(bitmap),384,1,48));assert(apx_printer_set_energy(300)==APX_PRINTER_BUSY);finish();
 assert(state().max_heat_count>=base_max*200/base_energy-2&&state().max_heat_count<=1112&&state().energy_percent==200);
 assert(!apx_printer_set_energy(100));
 assert(!apx_printer_feed(3));finish();assert(state().steps==3&&state().state==APX_PRINTER_DONE);
 assert(!apx_printer_feed(10));while(state().state!=APX_PRINTER_PRINTING)apx_printer_poll();
 unsigned old_steps=state().steps;paper_removed=1;apx_printer_poll();inactive();
 assert(state().error==APX_PRINTER_PAPER&&state().steps==old_steps);paper_removed=0;
 assert(!submit(bitmap,sizeof(bitmap),384,1,48));first_pulse();
 unsigned old_pulses=pulses;paper_removed=1;apx_printer_poll();inactive();
 assert(state().error==APX_PRINTER_PAPER&&pulses==old_pulses);paper_removed=0;
 assert(!submit(bitmap,sizeof(bitmap),384,1,48));first_pulse();
 apx_printer_cancel();inactive();assert(state().state==APX_PRINTER_CANCELLED);
 assert(!apx_printer_check());finish();assert(state().state==APX_PRINTER_DONE);
 assert(!submit(bitmap,sizeof(bitmap),384,1,48));apx_printer_cancel();assert(!apx_printer_busy()&&state().state==APX_PRINTER_CANCELLED);
 /* Lowering the limit exercises the overtemperature branch without heating. */
 assert(apx_printer_set_temperature_limit(66)==APX_PRINTER_INVALID);
 assert(!apx_printer_set_temperature_limit(10));old_pulses=pulses;
 assert(!apx_printer_check());assert(apx_printer_set_temperature_limit(65)==APX_PRINTER_BUSY);
 finish();assert(state().error==APX_PRINTER_TEMPERATURE&&state().temperature_valid&&pulses==old_pulses);
 assert(!apx_printer_set_temperature_limit(65));
 adc_value=385;assert(!apx_printer_feed(1));finish();assert(state().error==APX_PRINTER_TEMPERATURE);
 adc_value=0;assert(!apx_printer_feed(1));finish();assert(state().error==APX_PRINTER_TEMPERATURE);
 adc_value=1023;assert(!apx_printer_feed(1));finish();assert(state().error==APX_PRINTER_TEMPERATURE);
 adc_value=800;adc_fail=1;assert(!apx_printer_feed(1));finish();assert(state().error==APX_PRINTER_TIMEOUT);adc_fail=0;
 /* Raw probes may report invalid inputs but can never arm a normal job. */
 adc_value=0;old_pulses=pulses;assert(!apx_printer_probe_adc(1));finish();
 assert(state().state==APX_PRINTER_DONE&&!state().temperature_valid&&pulses==old_pulses&&state().steps==0);
 assert(!apx_printer_feed(1));finish();assert(state().error==APX_PRINTER_TEMPERATURE&&pulses==old_pulses);
 assert(apx_printer_probe_adc(12)==APX_PRINTER_INVALID);adc_value=800;
 battery_valid=0;assert(!submit(bitmap,sizeof(bitmap),384,1,48));finish();assert(state().error==APX_PRINTER_POWER);battery_valid=1;
 battery_mv=3300;assert(!submit(bitmap,sizeof(bitmap),384,1,48));finish();assert(state().error==APX_PRINTER_POWER);battery_mv=4200;
 timer_stuck=1;assert(!apx_printer_check());finish();assert(state().error==APX_PRINTER_IO);timer_stuck=0;
 timer_frequency=100000;assert(!apx_printer_check());finish();assert(state().error==APX_PRINTER_IO);timer_frequency=1000000;
 /* All-black: both half-rows have dots, so the second half-row pulses with the timer stuck. */
 memset(bitmap,0xff,sizeof(bitmap));
 assert(!submit(bitmap,sizeof(bitmap),384,1,48));first_pulse();timer_stuck=1;
 finish();assert(state().error==APX_PRINTER_TIMEOUT);timer_stuck=0;
 /* Loss during the first pulse must suppress the other 31 passes. */
 drop_at_pulse=pulses+1;assert(!submit(bitmap,sizeof(bitmap),384,1,48));finish();
 assert(state().error==APX_PRINTER_PAPER&&pulses==drop_at_pulse);paper_removed=0;drop_at_pulse=0;
 /* An inherited force-high must never be restored onto an actuator. */
 regs[0x8380]&=~0x200u;regs[0x81c0]|=0x200u;
 assert(!apx_printer_check());finish();assert(!(regs[0x81c0]&0x200));
 /* Centered non-byte width, mixed halves and multiple rows. */
 uint8_t narrow[]={0xa8,0x88,0x51,0x50,0x81,0x08};
 assert(!submit(narrow,sizeof(narrow),13,3,2));finish();assert(state().row==3&&state().steps==6);
 unsigned before=pulses;memset(bitmap,0,sizeof(bitmap));
 assert(!submit(bitmap,sizeof(bitmap),384,1,48));finish();assert(pulses==before);
 /* Photo completion adds the calibrated320 forward phases, with no heat. Verify
  * the actual four motor outputs stay in order through the image/feed seam. */
 assert(!submit_photo(bitmap,sizeof(bitmap),384,1,48));
 assert(submit_photo(bitmap,sizeof(bitmap),384,1,48)==APX_PRINTER_BUSY);
 unsigned previous_steps=0,iterations=0;
 const unsigned forward[]={6,10,9,5};
 while(apx_printer_busy()&&++iterations<2000000){
  apx_printer_poll();unsigned steps=state().steps;
  if(steps!=previous_steps){
   unsigned coils=(!!(pins[3]&1))|((!!(pins[4]&0x400))<<1)|((!!(pins[0]&8))<<2)|((!!(pins[4]&0x800))<<3);
   assert(steps==previous_steps+1&&coils==forward[(steps-1)&3]);previous_steps=steps;
  }
 }
 inactive();assert(state().state==APX_PRINTER_DONE&&state().row==1&&state().steps==322&&pulses==before);
 /* After real heating, cancellation and paper loss during finishing must
  * stop motion, keep heat off and avoid a delayed feed on the next job. */
 memset(bitmap,0xaa,sizeof(bitmap));assert(!submit_photo(bitmap,sizeof(bitmap),384,1,48));
 while(apx_printer_busy()&&!state().row)apx_printer_poll();
 assert(state().state==APX_PRINTER_PRINTING&&state().steps==3&&pulses>before);
 before=pulses;apx_printer_cancel();inactive();assert(state().state==APX_PRINTER_CANCELLED&&pulses==before);
 assert(!submit_photo(bitmap,sizeof(bitmap),384,1,48));
 while(apx_printer_busy()&&!state().row)apx_printer_poll();
 previous_steps=state().steps;before=pulses;paper_removed=1;apx_printer_poll();inactive();
 assert(state().error==APX_PRINTER_PAPER&&state().steps==previous_steps&&pulses==before);paper_removed=0;
 assert(!submit(bitmap,sizeof(bitmap),384,1,48));finish();assert(state().state==APX_PRINTER_DONE&&state().steps==2);
 /* Gray pixels retain32 temporal levels at the actual latch/strobe edges. */
 const uint8_t levels[]={0,8,128,248,255};
 assert(!apx_printer_set_mode(APX_PRINTER_GRAY));
 assert(apx_printer_submit_image(levels,4,5,1,5)==APX_PRINTER_INVALID);
 assert(!apx_printer_submit_image(levels,sizeof(levels),5,1,5));
 expected=levels;expected_w=expected_stride=5;expected_luma=1;memset(exposures,0,sizeof(exposures));
 assert(apx_printer_set_mode(APX_PRINTER_DOT)==APX_PRINTER_BUSY);before=pulses;
 while(state().state!=APX_PRINTER_PRINTING)apx_printer_poll();
 unsigned pre_shift=clocks;while(clocks-pre_shift<384)apx_printer_poll();
 /* Cross the raw14-bit reference wrap during real heating, after preflight
  * and first-plane shifting, and require successful completion. */
 ticks=((ticks+0x3fff)&~0x3fffu)+0x3ffc;finish();
 assert(state().state==APX_PRINTER_DONE&&state().steps==2&&pulses==before+63);
 /* Half-row timing counters; with a fixed row period the two half-rows are at least that far apart. */
 assert(state().period_count==1);
#if APX_PRINTER_ROW_PERIOD_GRAY_US>0
 assert(state().period_min>=(uint32_t)(((uint64_t)APX_PRINTER_ROW_PERIOD_GRAY_US*32768u+999999u)/1000000u));
#endif
 assert(state().period_hist[0]+state().period_hist[1]+state().period_hist[2]+state().period_hist[3]+state().period_hist[4]+state().period_hist[5]+state().period_hist[6]==1);
 const unsigned gray_exposures[]={32,31,16,1,0};
 for(unsigned x=0;x<384;++x)assert(exposures[x]==(x>=189&&x<194?gray_exposures[x-189]:0));
 uint8_t white[384];memset(white,255,sizeof(white));before=pulses;
 assert(!apx_printer_submit_image(white,sizeof(white),384,1,384));
 expected=white;expected_w=expected_stride=384;finish();assert(pulses==before&&state().steps==2);
 /* Native-stock golden: Dot23/60/97/134/171/208 -> black row, white row. */
 const uint8_t dot_source[]={23,60,97,134,171,208},dot_expected[]={0,0,0,255,255,255};
 assert(!apx_printer_set_mode(APX_PRINTER_DOT));
 assert(!apx_printer_submit_image(dot_source,sizeof(dot_source),3,2,3));
 expected=dot_expected;expected_w=expected_stride=3;before=pulses;finish();
 assert(state().state==APX_PRINTER_DONE&&state().row==2&&state().steps==4&&pulses==before+64);
 assert(!apx_printer_set_mode(APX_PRINTER_GRAY));
 assert(!apx_printer_submit_image(levels,sizeof(levels),5,1,5));
 expected=levels;expected_w=expected_stride=5;drop_at_pulse=pulses+1;finish();inactive();
 assert(state().error==APX_PRINTER_PAPER&&pulses==drop_at_pulse);paper_removed=drop_at_pulse=0;
 assert(!apx_printer_submit_image(levels,sizeof(levels),5,1,5));
 stall_at_pulse=pulses+1;finish();inactive();assert(state().error==APX_PRINTER_TIMEOUT);
 timer_stuck=stall_at_pulse=0;
 assert(!apx_printer_submit_image(levels,sizeof(levels),5,1,5));first_pulse();apx_printer_cancel();inactive();
 assert(state().state==APX_PRINTER_CANCELLED);
 assert(apx_printer_set_mode((enum apx_printer_mode)2)==APX_PRINTER_INVALID);
 /* Per-mode energy defaults, run-time override (100..400) and return to the default (0). */
 assert(!apx_printer_set_energy(0)&&!state().energy_override);
 assert(!apx_printer_set_mode(APX_PRINTER_GRAY)&&state().energy_percent==APX_PRINTER_ENERGY_GRAY_PERCENT);
 assert(!apx_printer_set_mode(APX_PRINTER_DOT)&&state().energy_percent==APX_PRINTER_ENERGY_DOT_PERCENT);
 assert(!apx_printer_set_energy(250)&&state().energy_override&&state().energy_percent==250);
 assert(!apx_printer_set_mode(APX_PRINTER_GRAY)&&state().energy_percent==250);
 assert(!apx_printer_set_mode(APX_PRINTER_DOT)&&state().energy_percent==250);
 assert(apx_printer_set_energy(99)==APX_PRINTER_INVALID&&apx_printer_set_energy(401)==APX_PRINTER_INVALID&&state().energy_percent==250);
 assert(!apx_printer_set_energy(0)&&!state().energy_override&&state().energy_percent==APX_PRINTER_ENERGY_DOT_PERCENT);
 assert(!apx_printer_set_mode(APX_PRINTER_GRAY)&&state().energy_percent==APX_PRINTER_ENERGY_GRAY_PERCENT);
 /* Luma protocol is separate from packed rows, validates dimensions,
  * sequential chunks and completion, and cannot overwrite a live job. */
 uint8_t input[16];put32(input,384);put32(input+4,129);
 assert(command(14,input,8)==APXD_RANGE);
 put32(input,5);put32(input+4,1);assert(!command(14,input,8));
 assert(command(16,NULL,0)==APXD_INVALID);
 put32(input,0);memcpy(input+4,levels,5);
 assert(command(4,input,9)==APXD_RANGE);
 put32(input,1);assert(command(15,input,9)==APXD_RANGE);
 put32(input,0);assert(!command(15,input,6));
 assert(command(15,input,6)==APXD_RANGE);
 put32(input,2);memcpy(input+4,levels+2,3);assert(!command(15,input,7));
 assert(command(5,NULL,0)==APXD_INVALID);
 assert(!command(16,NULL,0));expected=levels;expected_w=expected_stride=5;expected_luma=1;
 put32(input,APX_PRINTER_DOT);assert(command(13,input,4)==0x600+APX_PRINTER_BUSY);
 assert(command(14,input,8)==0x600+APX_PRINTER_BUSY);finish();
 assert(state().state==APX_PRINTER_DONE&&state().input_format==1&&state().mode==APX_PRINTER_GRAY);
 put32(input,2);assert(command(13,input,4)==0x600+APX_PRINTER_INVALID);
 put32(input,APX_PRINTER_DOT);assert(!command(13,input,4)&&state().mode==APX_PRINTER_DOT);
 assert(latches);
 assert(battery_deferrals);
 apx_printer_shutdown();assert(state().state==APX_PRINTER_OFF&&!head);
 puts("Printer model: actual GPIO/SPR edges, 384-bit shift/latch, raster parity/centering, blank control, pulse ordering, photo finishing, interlocks and cleanup passed");
}
