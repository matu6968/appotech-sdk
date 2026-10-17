#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/display.h>
#include <appotech/buttons.h>
#include <appotech/power.h>
#include <appotech/audio_files.h>
#include <appotech/microphone_files.h>
#include <appotech/camera_files.h>
#include <stdio.h>
#include <appotech/uart.h>
#include <appotech/battery.h>
#include <appotech/spi.h>
#include <appotech/trace.h>
/* Battery outline, five voltage segments; + indicates external supply.
 * Modify only the completed preview, never the photo/capture buffer. */
void apx_camera_preview_overlay(uint8_t *y,uint8_t *vu,unsigned pitch){
 struct apx_battery_info b;apx_battery_get_info(&b);
 for(unsigned row=4;row<18;++row)for(unsigned x=178;x<216;++x){
  unsigned edge=((row==6||row==15)&&x>=186&&x<214)||((x==186||x==213)&&row>=6&&row<=15)||(x>=214&&row>=9&&row<=12);
  unsigned segment=x>=188&&x<211&&row>=8&&row<=13&&((x-188)%5<3)&&((x-188)/5<b.level);
  unsigned plus=b.external_power&&((x==181&&row>=8&&row<=12)||(row==10&&x>=179&&x<=183));
  y[row*pitch+x]=edge||plus||(b.valid&&segment)?235:16;
 }
 for(unsigned row=2;row<9;++row)for(unsigned x=178;x<216;++x)vu[row*pitch+x]=128;
}
int main(void) {
 apx_trace(APX_TRACE_MAIN,0);
 apx_usb_init();apx_trace(APX_TRACE_APP_BASE,apx_ticks()&0xffffff);apx_trace_regs(2);apx_battery_probe(1);
 apx_buttons_init();apx_trace(APX_TRACE_APP_BASE+1,0);apx_battery_probe(2);
 apx_display_init();apx_trace(APX_TRACE_APP_BASE+2,0);apx_battery_probe(3);
 int battery=apx_battery_init(),spi=apx_spi_init();
 apx_trace(APX_TRACE_APP_BASE+3,(uint32_t)(battery&0xfff)<<12|(uint32_t)(spi&0xfff));apx_battery_probe(4);
 printf("Battery/SPI initialized: %d/%d\n",battery,spi);
 int audio=apx_audio_files_init();apx_trace(APX_TRACE_APP_BASE+4,(uint32_t)audio&0xffffff);
 int mic=apx_microphone_files_init();apx_trace(APX_TRACE_APP_BASE+5,(uint32_t)mic&0xffffff);apx_battery_probe(5);
 int camera=apx_camera_files_init();apx_trace(APX_TRACE_APP_BASE+6,(uint32_t)camera&0xffffff);apx_battery_probe(6);
 printf("Camera capture/preview initialized: %d; speaker/microphone %d/%d\n",camera,audio,mic);
 uint32_t power_since=0;unsigned held=0,armed=0;
 apx_trace(APX_TRACE_APP_BASE+7,apx_ticks()&0xffffff);
 for(;;){apx_debug_poll();apx_uart_poll();apx_buttons_poll();apx_battery_poll();apx_display_poll();apx_camera_files_poll();apx_maintenance();
  struct apx_buttons_info buttons;apx_buttons_get_info(&buttons);
  /* A camera powered on by holding Power still sees it held; only a release arms power-off. */
  if(!(buttons.state&APX_BUTTON_POWER))armed=1;
  if(armed&&(buttons.state&APX_BUTTON_POWER)){if(!held){power_since=apx_ticks();held=1;}
   else if(apx_ticks()-power_since>=65536){uint32_t result=apx_power_off();printf("Power off failed %lu\n",(unsigned long)result);power_since=apx_ticks();}}
  else held=0;
 }
}
