#include <appotech/camera.h>
#include <appotech/gpio.h>
#include <appotech/soc.h>
#include <assert.h>
#include <stdio.h>
void apx_exception_expect_reset(void) {}
static uint32_t regs[65536],ticks;
static unsigned nack,stuck,cleaned,invalidated,overlap,halts;
uint32_t apx_ticks(void) {return ticks++;}
void apx_delay_ms(uint32_t ms) {ticks+=ms*33;}
uint32_t apx_spr_read(uint32_t reg) {return regs[reg];}
void apx_spr_write(uint32_t reg,uint32_t value) {
 if(reg==0xb000&&!(value&1))++halts;
 if(reg==0x90a0){
  if(value&1)regs[0x90a4]&=~1u;
  else if(!stuck&&(value&0x78))regs[0x90a4]=1|(nack?128:0);
  regs[reg]=0;return;
 }
 if(reg==0xb00c){regs[reg]&=~value;return;}
 regs[reg]=value;
}
int apx_gpio_configure(unsigned a,uint32_t b,enum apx_gpio_direction c,enum apx_gpio_pull d,uint32_t e) {
 (void)a;(void)b;(void)c;(void)d;(void)e;return 0;
}
int apx_gpio_write(unsigned a,uint32_t b,uint32_t c) {(void)a;(void)b;(void)c;return 0;}
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
 nack=1;assert(apx_camera_init()==APX_CAMERA_NACK);apx_camera_shutdown();nack=0;
 stuck=1;assert(apx_camera_init()==APX_CAMERA_TIMEOUT);apx_camera_shutdown();
 puts("Camera model: native/scaled DMA bounds, events, one-shot handoff, rearm, deadlines, NACK and clock restoration passed");
}
