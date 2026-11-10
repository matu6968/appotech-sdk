#include <appotech/camera.h>
#include <appotech/gpio.h>
#include <appotech/soc.h>
#include <assert.h>
#include <stdio.h>
#include "board.h"
static unsigned printer_busy,pdn=0xc00,pdn_both;
int apx_printer_busy(void){return printer_busy;}
void apx_exception_expect_reset(void) {}
static uint32_t regs[65536],ticks;
static unsigned nack,stuck,cleaned,invalidated,overlap,halts,first_byte,bus_address,bus_index,sensor_reg;
static uint8_t sensor_values[256],paged[3][256],cur_page;
static unsigned sensor_writes;
uint32_t apx_ticks(void) {return ticks++;}
void apx_delay_ms(uint32_t ms) {ticks+=ms*33;}
void apx_delay_service(void) {}
uint32_t apx_spr_read(uint32_t reg) {return regs[reg];}
void apx_spr_write(uint32_t reg,uint32_t value) {
 if(reg==0xb000&&!(value&1))++halts;
 if(reg==0x90a0){
  if(value==0x80){first_byte=1;bus_index=0;}
  if(value&0x10) {
   if(bus_index==1)sensor_reg=regs[0x909c]&255;
   if(bus_index==2&&!(bus_address&1)){
    sensor_values[sensor_reg]=regs[0x909c]&255;++sensor_writes;
    if(sensor_reg==0xfd)cur_page=regs[0x909c]&3;else paged[cur_page%3][sensor_reg]=regs[0x909c]&255;
   }
   ++bus_index;
  }
  if((value&0x10)&&first_byte){bus_address=regs[0x909c];first_byte=0;}
  unsigned wrong_address=0;
#if APX_BOARD_SILVERGEAR
  unsigned expected=regs[0x9098]==0xa6?0xdc:0x42;
  wrong_address=(bus_address&~1u)!=expected;
#endif
  if(value&1)regs[0x90a4]&=~1u;
  else if(!stuck&&(value&0x78))regs[0x90a4]=1|((nack||wrong_address)?128:0);
  regs[reg]=0;return;
 }
 if(reg==0xb00c){regs[reg]&=~value;return;}
 regs[reg]=value;
}
int apx_gpio_configure(unsigned a,uint32_t b,enum apx_gpio_direction c,enum apx_gpio_pull d,uint32_t e) {
 (void)c;(void)d;if(a==4){pdn=(pdn&~b)|(e&b);pdn_both+=pdn==0xc00;}return 0;
}
int apx_gpio_write(unsigned a,uint32_t b,uint32_t c) {if(a==4)pdn=(pdn&~b)|(c&b);return 0;}
int apx_audio_buffer_busy(const void *p,size_t n) {(void)p;(void)n;return overlap;}
int apx_mic_buffer_busy(const void *p,size_t n) {(void)p;(void)n;return 0;}
int apx_dma_clean(const void *p,size_t n) {(void)p;(void)n;++cleaned;return 0;}
int apx_dma_invalidate(void *p,size_t n) {(void)p;(void)n;++invalidated;return 0;}
int main(void) {
 void *frame=(void *)0x022e0020;struct apx_camera_info i;
 regs[0x884c]=0x8b123456;regs[0x8348]=0x12345678;regs[0x9098]=0x39;
 assert(apx_camera_capture_start((void *)0x022e0021,460800)==APX_CAMERA_RANGE);
 assert(apx_camera_capture_start(frame,460799)==APX_CAMERA_RANGE);
 assert(apx_camera_capture_start((void *)0x023ffff0,460800)==APX_CAMERA_RANGE);
 overlap=1;assert(apx_camera_capture_start(frame,460800)==APX_CAMERA_BUSY);overlap=0;
 assert(!apx_camera_init());assert(regs[0x884c]==0x8b122b56);
 assert(!apx_camera_capture_start(frame,460800));assert(regs[0xb050]==(uintptr_t)frame&&regs[0xb054]==(uintptr_t)frame+307200);
 assert(apx_camera_capture_start(frame,460800)==APX_CAMERA_BUSY);
 assert(apx_camera_buffer_busy(frame,1)&&!apx_camera_buffer_busy((void *)0x023f0000,32));
 regs[0xb00c]=8;apx_camera_poll();apx_camera_get_info(&i);assert(i.state==APX_CAMERA_CAPTURING);
 regs[0xb00c]=9;apx_maintenance();apx_camera_get_info(&i);
 assert(i.state==APX_CAMERA_DONE&&i.completed==1&&cleaned&&invalidated==2&&!(regs[0xb000]&1));
 assert(!regs[0xb050]&&!regs[0xb054]&&!apx_camera_buffer_busy(frame,1));
 assert(!apx_camera_capture_start(frame,460800));ticks+=32769;apx_camera_poll();apx_camera_get_info(&i);
 assert(i.state==APX_CAMERA_ERROR&&i.timeouts==1&&!regs[0xb050]);
 apx_camera_stop();
 assert(apx_camera_scaled_start(frame,59135,220,176,224)==APX_CAMERA_RANGE);
 assert(apx_camera_scaled_start(frame,59136,221,176,224)==APX_CAMERA_RANGE);
 assert(apx_camera_scaled_start(frame,59136,220,177,224)==APX_CAMERA_RANGE);
 assert(apx_camera_scaled_start(frame,59136,220,176,220)==APX_CAMERA_RANGE);
 assert(apx_camera_scaled_start(frame,59136,0xffffffffu,176,224)==APX_CAMERA_RANGE);
 overlap=1;assert(apx_camera_scaled_start(frame,59136,220,176,224)==APX_CAMERA_BUSY);overlap=0;
 assert(!apx_camera_scaled_start(frame,59136,220,176,224));unsigned before=halts;
 assert(regs[0xb004]==0x420000&&regs[0xb030]==(640u<<16)&&regs[0xb034]==(480u<<16));
 assert(regs[0xb180]==0x00b000dc&&regs[0xb184]==(640u<<16)/110&&regs[0xb188]==(480u<<16)/88);
 assert(regs[0xb068]==224&&regs[0xb060]==(uintptr_t)frame&&regs[0xb064]==(uintptr_t)frame+224*176);
 assert(apx_camera_scaled_start(frame,59136,220,176,224)==APX_CAMERA_BUSY);
 regs[0xb00c]=9;apx_camera_poll();apx_camera_get_info(&i);assert(i.state==APX_CAMERA_CAPTURING);
 regs[0xb00c]=10;apx_camera_poll();apx_camera_get_info(&i);assert(i.state==APX_CAMERA_CAPTURING);
 regs[0xb018]&=~(1u<<18);apx_maintenance();apx_camera_get_info(&i);
 assert(i.state==APX_CAMERA_DONE&&i.scaled&&i.bytes==59136&&i.width==220&&i.pitch==224&&i.height==176);
 assert(halts==before&&(regs[0xb000]&1)&&!regs[0xb060]&&!regs[0xb064]&&!apx_camera_buffer_busy(frame,1));
 assert(!apx_camera_scaled_start((void *)0x02300000,59136,220,176,224));assert(halts==before);
 regs[0xb00c]=10;ticks+=32769;apx_camera_poll();apx_camera_get_info(&i);
 assert(i.state==APX_CAMERA_ERROR&&i.timeouts==2&&!regs[0xb060]&&!(regs[0xb000]&1));
 apx_camera_stop();assert(!apx_camera_scaled_start(frame,59136,220,176,224));
 regs[0xb00c]=0x40;apx_camera_poll();apx_camera_get_info(&i);assert(i.error==APX_CAMERA_HARDWARE&&!regs[0xb060]);
 apx_camera_stop();assert(!apx_camera_scaled_start(frame,115200,320,240,320));
 regs[0xb00c]=2;regs[0xb018]&=~(1u<<18);apx_camera_poll();
 assert(!apx_camera_capture_start(frame,460800)&&!regs[0xb004]&&!regs[0xb060]&&regs[0xb018]==0x80000);
 apx_camera_get_info(&i);assert(!i.scaled&&i.width==640&&i.height==480&&i.pitch==640);
 apx_camera_shutdown();assert(regs[0x884c]==0x8b123456&&regs[0x8348]==0x12345678);
 #if APX_BOARD_SILVERGEAR
 for(unsigned n=0;n<10;++n) {
  assert(!apx_camera_select(n&1));apx_camera_get_info(&i);
  assert(i.selected==(n&1)&&i.count==2&&i.sensor_id==0x39);
  assert(pdn==((n&1)?0x800u:0x400u)&&pdn_both>=n+1);
  assert(sensor_values[0x31]==((n&1)?2:0));
  assert(!apx_camera_capture_start(frame,460800));
  assert(apx_camera_select((n&1)^1)==APX_CAMERA_BUSY);
  apx_camera_stop();apx_camera_shutdown();
 }
 regs[0x9098]=0xa6;assert(!apx_camera_select(APX_CAMERA_FRONT));apx_camera_get_info(&i);
 assert(i.sensor_id==0xa6&&i.native_width==640&&i.native_height==480&&bus_address==0xdc);
 assert(regs[0x9090]==143&&pdn==0x800&&sensor_values[0x4a]==12);
 apx_camera_shutdown();regs[0x9098]=0x39;
 printer_busy=1;assert(apx_camera_init()==APX_CAMERA_BUSY);printer_busy=0;
 assert(apx_camera_select(2)==APX_CAMERA_UNSUPPORTED);
#endif
 nack=1;assert(apx_camera_init()==APX_CAMERA_NACK);apx_camera_shutdown();nack=0;
 stuck=1;assert(apx_camera_init()==APX_CAMERA_TIMEOUT);apx_camera_shutdown();
 /* Sensor controls: stored per camera, written by apx_camera_service(), page 0 left selected, written back after a new init */
 {
  stuck=0;assert(!apx_camera_init());apx_camera_get_info(&i);const enum apx_camera_id cam=(enum apx_camera_id)i.selected; /* whichever camera the steps above left selected */
  struct apx_camera_controls c;apx_camera_controls_default(&c);
  c.brightness=0xa0;c.gain=100;c.contrast=50;c.auto_exposure=0;c.auto_white_balance=0;c.exposure_lines=300;c.dynamic_frame_rate=0;c.frame_interval_us=40000;c.blank_lines=12;
  unsigned w0=sensor_writes;
  assert(!apx_camera_controls_set(cam,&c)&&sensor_writes==w0); /* storing touches nothing */
  assert(!apx_camera_service());
  assert(paged[1][0x18]==0xa0&&paged[1][0x04]==100&&paged[1][0x12]==50&&paged[1][0x02]==3); /* 40 ms = 600 lines -> 3 x 151 */
  assert(!(paged[1][0x32]&0x11)&&paged[0][0x03]==1&&paged[0][0x04]==44&&paged[0][0x06]==12&&cur_page==0);
  w0=sensor_writes;assert(!apx_camera_service()&&sensor_writes==w0); /* nothing changed: nothing written */
  paged[0][0x03]=0xff; /* the sensor's last auto-exposure update overwrites the exposure ... */
  ticks+=4000;assert(!apx_camera_service()&&paged[0][0x03]==1&&paged[0][0x04]==44); /* ... and it is written again about 100 ms later */
  w0=sensor_writes;assert(!apx_camera_service()&&sensor_writes==w0);
  struct apx_camera_controls g;apx_camera_controls_get(cam,&g);assert(g.brightness==0xa0&&!g.auto_exposure);
  c.gain=10;c.contrast=250;c.exposure_lines=5000;assert(!apx_camera_controls_set(cam,&c));apx_camera_controls_get(cam,&g);
  assert(g.gain==32&&g.contrast==160&&g.exposure_lines==2047); /* clamped */
  assert(apx_camera_controls_set((enum apx_camera_id)5,&c)==APX_CAMERA_INVALID&&apx_camera_controls_set(cam,0)==APX_CAMERA_INVALID);
  apx_camera_shutdown();assert(!apx_camera_init()); /* the register table has reset the sensor: the chosen controls come back */
  assert(paged[1][0x18]==0xa0&&paged[1][0x04]==32&&paged[1][0x12]==160&&paged[0][0x06]==12&&!(paged[1][0x32]&0x11)&&cur_page==0);
  unsigned v=0;assert(!apx_camera_sensor_read(1,0x18,&v)&&cur_page==0&&apx_camera_sensor_read(3,0,&v)==APX_CAMERA_RANGE&&apx_camera_sensor_read(0,0x100,&v)==APX_CAMERA_RANGE);
 }
 puts("Camera model: native/scaled DMA bounds, events, one-shot handoff, rearm, deadlines, NACK and clock restoration passed");
}
