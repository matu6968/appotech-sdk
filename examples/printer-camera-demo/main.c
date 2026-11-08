#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/display.h>
#include <appotech/buttons.h>
#include <appotech/camera.h>
#include <appotech/camera_files.h>
#include <appotech/printer.h>
#include <appotech/battery.h>
#include <appotech/power.h>
#include <appotech/spi.h>
#include <stdio.h>
#include <string.h>
#include "font.h"
#include "../../src/printer_image.h"
#ifndef APX_PRINTER_GRAY_COMPARE
#define APX_PRINTER_GRAY_COMPARE 0
#endif

/* The printer borrows the frozen camera frame; its short edge spans the head
 * and its long edge follows paper feed. Rendering uses only a few row buffers. */
static unsigned frozen,printing,print_pending,diagnostics,selected,camera_pending=2;
static uint32_t last_keys;
/* A 2 s Power hold arms power-off; the supply is cut once Power is released (stock waits for the key ladder to read
 * "no key" first, otherwise the held key restarts the board). `apxtool debug bootrom` is the BootROM hand-off. */
static unsigned power_off_pending;
#if APX_PRINTER_GRAY_COMPARE
static unsigned compare_phase;
static uint32_t compare_hash;
static uint32_t frame_hash(const uint8_t *frame,size_t bytes){
 uint32_t hash=2166136261u;
 for(size_t n=0;n<bytes;++n){hash=(hash^frame[n])*16777619u;if(!(n&4095)){apx_battery_poll();apx_maintenance();}}
 return hash;
}
#endif
static void text(uint8_t *y,unsigned pitch,unsigned x,unsigned row,const char *s) {
 for(;*s&&x+5<APX_DISPLAY_WIDTH;++s,x+=6) {
  const uint8_t *g=font[glyph(*s)];
  for(unsigned dy=0;dy<7;++dy)for(unsigned dx=0;dx<5;++dx)
   y[(row+dy)*pitch+x+dx]=(g[dx]&(1u<<dy))?235:16;
 }
}
void apx_camera_preview_overlay(uint8_t *y,uint8_t *vu,unsigned pitch) {
 char line[64];struct apx_buttons_info keys;apx_buttons_get_info(&keys);
 struct apx_camera_info camera;apx_camera_get_info(&camera);selected=camera.selected;
 if(diagnostics) {
  memset(y,32,pitch*APX_DISPLAY_HEIGHT);memset(vu,128,pitch*APX_DISPLAY_HEIGHT/2);
  static const char *labels[]={"SHUTTER","POWER","PRINT","MODE","LEFT","RIGHT","EXIT"};
  static const unsigned flags[]={APX_BUTTON_SHUTTER,APX_BUTTON_POWER,APX_BUTTON_PRINT,APX_BUTTON_MODE,APX_BUTTON_LEFT,APX_BUTTON_RIGHT,APX_BUTTON_EXIT};
  for(unsigned n=0;n<7;++n){snprintf(line,sizeof(line),"%s %s",labels[n],keys.state&flags[n]?"PRESSED":"");text(y,pitch,4,40+n*14,line);}
 } else {
  memset(y,16,pitch*24);memset(vu,128,pitch*12);
 }
 if(power_off_pending)snprintf(line,sizeof(line),"RELEASE POWER TO TURN OFF");
 else snprintf(line,sizeof(line),"%s %s%s",selected?"FRONT":"REAR",frozen?"FROZEN PRINT TO PRINT":"LIVE SHUTTER TO CAPTURE",
  keys.led?"  FLASH ON":"");
 text(y,pitch,4,3,line);
 if(diagnostics)snprintf(line,sizeof(line),"ADC %u KEYS %lu",keys.adc,(unsigned long)last_keys);
 else {struct apx_printer_info printer;apx_printer_get_info(&printer);
  snprintf(line,sizeof(line),"%s PRINT   MODE TO CHANGE",printer.mode==APX_PRINTER_DOT?"DOT":"GRAY");}
 text(y,pitch,4,14,line);
 unsigned bottom=APX_DISPLAY_HEIGHT-20;
 memset(y+bottom*pitch,16,20*pitch);memset(vu+bottom/2*pitch,128,10*pitch);
 text(y,pitch,4,bottom+2,"LEFT RIGHT CAMERA  POWER FLASH  HOLD MODE DIAG");
 text(y,pitch,4,bottom+12,"HOLD PRINT FEED  EXIT LIVE OR CANCEL");
}
static unsigned camera_command(unsigned cmd,const uint8_t *in,size_t len) {
 uint8_t out[1];size_t n;return apx_camera_command(cmd,in,len,out,&n);
}
int apx_printer_photo_cancel(void) {unsigned pending=print_pending;print_pending=0;return pending;}
int apx_printer_photo_request(void) {
 if(printing||print_pending)return APX_PRINTER_BUSY;
 const uint8_t *frame;size_t bytes;
 if(apx_camera_files_raw_frame(&frame,&bytes)) {
  unsigned e=camera_command(6,NULL,0);if(e)return APX_PRINTER_BUSY;
 }
 print_pending=1;return 0;
}
int main(void) {
 apx_usb_init();int power=apx_power_domain_enable(),battery=apx_battery_init();
 printf("board power %d battery %d\n",power,battery);
 apx_buttons_init();apx_display_init();
 /* SPI NOR (944016, 4 MiB) for `apxtool debug flash info/read/install` from the running image. */
 printf("SPI flash init %d\n",apx_spi_init());
 int e=apx_camera_files_init();printf("Silvergear preview init %d; MODE selects Gray/Dot; hold MODE for diagnostics\n",e);
 unsigned power_armed=0,power_held=0,print_held=0,feed_pending=0,mode_held=0,shown_mode=APX_PRINTER_GRAY;
 uint32_t power_since=0,print_since=0,mode_since=0,click_since=0;
 unsigned click_armed=0;
 for(;;) {
  apx_debug_poll();apx_buttons_poll();apx_battery_poll();apx_display_poll();
  apx_printer_poll();if(!printing)apx_camera_files_poll();apx_maintenance();
  struct apx_printer_info printer;apx_printer_get_info(&printer);
  if(printer.mode!=shown_mode){shown_mode=printer.mode;apx_camera_files_redraw();}
  struct apx_buttons_info keys;apx_buttons_get_info(&keys);last_keys=keys.state;
  if(!(keys.state&APX_BUTTON_POWER)) {
   power_armed=1;power_held=0;
   if(power_off_pending) { /* released after the 2 s hold: cut the supply (returns only on failure) */
    uint32_t result=apx_power_off();printf("Power off failed %lu\n",(unsigned long)result);
    power_off_pending=0;
   }
  } else if(power_armed) {
   if(!power_held){power_since=apx_ticks();power_held=1;}
   else if(!power_off_pending&&apx_ticks()-power_since>=65536){(void)apx_led_set(0);power_off_pending=1;}
  }
  struct apx_button_event event;
  while(apx_buttons_next(&event)) {
   printf("key adc=%u pressed=%lu released=%lu\n",keys.adc,(unsigned long)event.pressed,(unsigned long)event.released);
   /* Stock toggles the flash LED on a Power click (event 37); a click is a press
    * shorter than one second that started after Power was released once. */
   if((event.pressed&APX_BUTTON_POWER)&&power_armed){click_armed=1;click_since=event.ticks;}
   if(event.released&APX_BUTTON_POWER){
    if(click_armed&&event.ticks-click_since<32768)(void)apx_led_set(apx_led_get()?0:1);
    click_armed=0;
   }
   if(!printing&&!print_pending&&(event.pressed&APX_BUTTON_MODE)){mode_held=1;mode_since=apx_ticks();}
   if((event.released&APX_BUTTON_MODE)&&mode_held){
    if(mode_held==1){
     if(diagnostics)diagnostics=0;
     else {struct apx_printer_info printer;apx_printer_get_info(&printer);
      (void)apx_printer_set_mode((enum apx_printer_mode)(printer.mode^1u));}
     apx_camera_files_redraw();
    }
    mode_held=0;
   }
   if(diagnostics)continue;
   if(event.pressed&APX_BUTTON_EXIT) {
    apx_printer_cancel();print_pending=print_held=feed_pending=0;
    if(!printing){(void)apx_camera_files_init();frozen=0;}
   }
   if(!printing&&!print_pending&&(event.pressed&(APX_BUTTON_LEFT|APX_BUTTON_RIGHT)))
    camera_pending=selected^1u;
   if(!printing&&!print_pending&&(event.pressed&APX_BUTTON_SHUTTER)) {
    if(frozen){(void)apx_camera_files_init();frozen=0;}else (void)camera_command(6,NULL,0);
   }
   if(!printing&&!print_pending&&(event.pressed&APX_BUTTON_PRINT)){print_held=1;print_since=apx_ticks();}
   if((event.released&APX_BUTTON_PRINT)&&print_held) {
    if(print_held==1&&!printing&&!print_pending){if(frozen)print_pending=1;else (void)camera_command(6,NULL,0);}
    print_held=0;
   }
  }
  if(mode_held==1&&!printing&&!print_pending&&apx_ticks()-mode_since>=32768){
   diagnostics=!diagnostics;mode_held=2;apx_camera_files_redraw();
  }
  if(print_held==1&&!printing&&!print_pending&&apx_ticks()-print_since>=32768){feed_pending=1;print_held=2;}
  if(feed_pending&&apx_display_status()==APX_DISPLAY_READY&&!apx_camera_files_suspend()) {
   e=apx_printer_feed(32);printing=!e;feed_pending=0;
   if(e){printf("paper feed start %d\n",e);(void)apx_camera_files_init();frozen=0;}
  }
  const uint8_t *frame;size_t bytes;
  if(!printing&&!apx_camera_files_raw_frame(&frame,&bytes)&&!frozen){frozen=1;apx_camera_files_redraw();}
  if(camera_pending<apx_camera_count()&&!printing&&apx_display_status()==APX_DISPLAY_READY) {
   uint8_t id[4]={camera_pending,0,0,0};unsigned status=camera_command(8,id,4);
   if(!status){selected=camera_pending;frozen=0;camera_pending=2;}
   else {printf("camera select error %lu\n",(unsigned long)status);camera_pending=2;}
  }
  if(print_pending&&frozen&&apx_display_status()==APX_DISPLAY_READY) {
   if(apx_camera_files_suspend())continue;
   if(apx_camera_files_raw_frame(&frame,&bytes)){print_pending=0;continue;}
#if APX_PRINTER_GRAY_COMPARE
   (void)apx_printer_set_mode(APX_PRINTER_GRAY);compare_hash=frame_hash(frame,bytes);
   printf("Gray comparison stock curve frame FNV32=%08lx\n",(unsigned long)compare_hash);
   e=apx_printer_submit_photo_luma_curve(frame,bytes,640,480,640,384,512,1);compare_phase=!e;
#else
   e=apx_printer_submit_photo_luma(frame,bytes,640,480,640,384,512);
#endif
   printf("photo print start %d\n",e);print_pending=0;printing=!e;
   if(e){(void)apx_camera_files_init();frozen=0;}
  }
  if(printing&&!apx_printer_busy()) {
   struct apx_printer_info p;apx_printer_get_info(&p);
   printf("print ended state=%lu error=%lu rows=%lu temperature_adc=%lu\n",(unsigned long)p.state,(unsigned long)p.error,(unsigned long)p.row,(unsigned long)p.temperature_adc);
#if APX_PRINTER_GRAY_COMPARE
   if(compare_phase==1&&p.state==APX_PRINTER_DONE){
    compare_phase=2;
    if(!apx_camera_files_raw_frame(&frame,&bytes)&&frame_hash(frame,bytes)==compare_hash){
     printf("Gray comparison darker curve SAME frame FNV32=%08lx\n",(unsigned long)compare_hash);
     e=apx_printer_submit_photo_luma_curve(frame,bytes,640,480,640,384,512,2);
     if(!e)continue;
    }
    printf("Gray comparison stopped: frame changed or submit failed\n");
   }
   compare_phase=0;
#endif
   printing=0;frozen=0;(void)apx_camera_files_init();
  }
 }
}
