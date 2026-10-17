#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/display.h>
#include <appotech/buttons.h>
#include <appotech/power.h>
#include <appotech/jpeg_files.h>
#include <appotech/audio_files.h>
#include <appotech/microphone_files.h>
#include <stdio.h>
int main(void) {
 apx_usb_init();apx_buttons_init();apx_display_init();apx_jpeg_files_init();
 int error=apx_audio_files_init(),mic_error=apx_microphone_files_init();
 printf("Audio: speaker and independent microphone PCM/WAV, initialization %d/%d\n",error,mic_error);
 uint32_t power_since=0;unsigned held=0,initial=0,armed=0;
 for(;;){apx_debug_poll();apx_buttons_poll();apx_display_poll();apx_jpeg_files_poll();apx_maintenance();
  struct apx_buttons_info buttons;apx_buttons_get_info(&buttons);
  /* A camera powered on by holding Power still sees it held; only a release arms power-off. */
  if(!(buttons.state&APX_BUTTON_POWER))armed=1;
  if(armed&&(buttons.state&APX_BUTTON_POWER)){if(!held){power_since=apx_ticks();held=1;}
   else if(apx_ticks()-power_since>=65536){uint32_t result=apx_power_off();printf("Power off failed %lu\n",(unsigned long)result);power_since=apx_ticks();}}
  else held=0;
  if(!initial&&apx_display_status()==APX_DISPLAY_READY){initial=1;
   uint32_t result=apx_jpeg_preview_file("0:/SDKJPEG/BASELINE/jpeg-test-220x176.jpg");printf("Initial JPEG preview: %lu\n",(unsigned long)result);}
 }
}
