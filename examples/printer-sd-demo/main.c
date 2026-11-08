/* Print a JPEG from the SD card in Gray or Dot mode (Silvergear printer camera).
 *
 * Reads 0:/PRINT.JPG, decodes it with the hardware JPEG engine (baseline 4:2:0 or
 * 4:2:2, fitted inside 512x384 - the stock photo geometry - and never upscaled), runs the stock
 * photo preparation for the selected mode (sharpen / contrast stretch / 16-level quantise for
 * Gray, contrast stretch for Dot; src/photo_prepare.c) and prints the luma through the same photo
 * path as printer-camera-demo: clockwise rotation, stock photo curve, 384-dot head, tear-off
 * feed. No camera and no LCD are used, because the printer shares pads with both; progress is
 * reported on the APXD log (UART/host).
 *
 * Keys: MODE toggles GRAY/DOT, PRINT prints, hold PRINT feeds paper,
 *       EXIT cancels a print or reloads the file, hold POWER leaves to the BootROM.
 * The SD card is only read. */
#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/buttons.h>
#include <appotech/camera.h>
#include <appotech/jpeg.h>
#include <appotech/printer.h>
#include <appotech/storage.h>
#include <appotech/battery.h>
#include <appotech/power.h>
#include <stdio.h>
#include <string.h>
#include "../../src/printer_image.h"
#include "../../src/photo_prepare.h"

#ifndef APX_PRINTER_SD_BOTH
#define APX_PRINTER_SD_BOTH 0   /* 1: one PRINT press prints Gray, then Dot, from the same decoded frame */
#endif
#ifndef APX_SDPRINT_PATH
#define APX_SDPRINT_PATH "0:/PRINT.JPG"
#endif
/* Decode limits: the stock photo buffer is 512x384 for 4:3 images. */
#ifndef APX_SDPRINT_MAX_WIDTH
#define APX_SDPRINT_MAX_WIDTH 512u
#endif
#ifndef APX_SDPRINT_MAX_HEIGHT
#define APX_SDPRINT_MAX_HEIGHT 384u
#endif
#if APX_SDPRINT_MAX_WIDTH>APX_JPEG_LARGE_MAX_WIDTH||APX_SDPRINT_MAX_HEIGHT>APX_JPEG_LARGE_MAX_HEIGHT
#error decode limits exceed the large decoder range
#endif
/* 1: run the stock photo preparation stages before printing; 0: print the decoded luma as it is
 * (the behaviour before the stages were ported), for A/B comparison on paper. */
#ifndef APX_SDPRINT_PREPARE
#define APX_SDPRINT_PREPARE 1
#endif

#define STREAM_BYTES (320u*1024u)
#define FRAME_PITCH ((APX_SDPRINT_MAX_WIDTH+31u)&~31u)
#define FRAME_BYTES (FRAME_PITCH*APX_SDPRINT_MAX_HEIGHT*3u/2u)
#define WORK_BYTES (APX_SDPRINT_MAX_WIDTH*APX_SDPRINT_MAX_HEIGHT)
/* Codec RAM is 1152 KiB: 320 KiB JPEG stream, 288 KiB NV21 frame, a packed luma copy that the stock
 * stages modify (rebuilt from the frame for every print, so Gray and Dot see the same source) and
 * their scratch memory, plus the SDK's own codec-RAM statics. */
static uint8_t stream[STREAM_BYTES] __attribute__((section(".jpeg_buffer"),aligned(32)));
static uint8_t frame[FRAME_BYTES] __attribute__((section(".jpeg_buffer"),aligned(32)));
static uint8_t work[WORK_BYTES] __attribute__((section(".jpeg_buffer"),aligned(32)));
static uint8_t scratch[WORK_BYTES] __attribute__((section(".jpeg_buffer"),aligned(32)));

/* No camera driver is linked: printer.c only asks for the selected-camera state. */
void apx_camera_get_info(struct apx_camera_info *out) {memset(out,0,sizeof(*out));}

static unsigned loaded,frame_w,frame_h,frame_pitch,frame_oh,printing,second_pending,feed_busy;
static uint32_t frame_hash,print_started;
static enum apx_printer_mode mode=APX_PRINTER_GRAY;

static uint32_t fnv(const uint8_t *p,size_t n) {
 uint32_t h=2166136261u;
 for(size_t i=0;i<n;++i){h=(h^p[i])*16777619u;if(!(i&4095)){apx_usb_poll();apx_maintenance();}}
 return h;
}
static void cooperate(void) {apx_usb_poll();apx_buttons_poll();apx_battery_poll();apx_maintenance();}
/* The stock stages run for seconds: keep USB, buttons and the tick accounting alive between rows. */
void apx_photo_yield(void) {cooperate();}

/* Read the whole file into `stream`, then zero 32 guard bytes as the decoder requires. */
static int read_file(size_t *bytes) {
 FRESULT r=apx_storage_mount();if(r){printf("sd mount failed: FatFs %d\n",(int)r);return -1;}
 FIL file;r=f_open(&file,APX_SDPRINT_PATH,FA_READ);
 if(r){printf("open %s failed: FatFs %d\n",APX_SDPRINT_PATH,(int)r);return -1;}
 FSIZE_t size=f_size(&file);size_t got_total=0;
 if(size<4||size>STREAM_BYTES-32){printf("file size %lu outside 4..%u\n",(unsigned long)size,STREAM_BYTES-32);r=FR_INVALID_PARAMETER;}
 while(!r&&got_total<size) {
  UINT n=size-got_total>4096?4096:(UINT)(size-got_total),got=0;
  r=f_read(&file,stream+got_total,n,&got);if(!r&&got!=n)r=FR_DISK_ERR;
  got_total+=got;cooperate();
 }
 FRESULT close=f_close(&file);if(!r)r=close;
 if(r){printf("read failed: FatFs %d after %lu bytes\n",(int)r,(unsigned long)got_total);return -1;}
 memset(stream+got_total,0,32);*bytes=got_total;return 0;
}

/* Fit inside the decode limits with the source aspect ratio, even sizes, no upscaling. */
static void fit(unsigned w,unsigned h,unsigned *ow,unsigned *oh) {
 unsigned fw=w,fh=h;
 if(fw>APX_SDPRINT_MAX_WIDTH){fh=(unsigned)((uint64_t)fh*APX_SDPRINT_MAX_WIDTH/fw);fw=APX_SDPRINT_MAX_WIDTH;}
 if(fh>APX_SDPRINT_MAX_HEIGHT){fw=(unsigned)((uint64_t)fw*APX_SDPRINT_MAX_HEIGHT/fh);fh=APX_SDPRINT_MAX_HEIGHT;}
 *ow=fw&~1u;*oh=fh&~1u;
}

static int load(void) {
 loaded=0;second_pending=0;
 size_t bytes;if(read_file(&bytes))return -1;
 struct apx_jpeg_info info;int e=apx_jpeg_inspect(stream,bytes,&info);
 if(e){printf("JPEG rejected: error %d (baseline 4:2:0/4:2:2, 8-bit, <=2048x1536 only)\n",e);return -1;}
 unsigned w,h;fit(info.width,info.height,&w,&h);
 if(w<2||h<2){printf("image too small to decode\n");return -1;}
 e=apx_jpeg_decode_start_large(stream,bytes,frame,sizeof(frame),w,h);
 if(e){printf("decode start error %d for %ux%u -> %ux%u\n",e,(unsigned)info.width,(unsigned)info.height,w,h);return -1;}
 for(;;) {
  apx_jpeg_poll();apx_jpeg_get_info(&info);
  if(info.state==APX_JPEG_DONE||info.state==APX_JPEG_ERROR)break;
  cooperate();
 }
 if(info.state==APX_JPEG_ERROR){printf("decode failed: error %lu status 0x%08lx\n",(unsigned long)info.error,(unsigned long)info.status);apx_jpeg_shutdown();return -1;}
 apx_jpeg_shutdown();
 frame_w=w;frame_h=h;frame_pitch=info.pitch;
 frame_oh=(unsigned)((uint64_t)APX_PRINTER_WIDTH*w/h);if(frame_oh&1)++frame_oh;
 if(frame_oh<2||frame_oh>4096){printf("aspect ratio gives %u print rows, outside 2..4096\n",frame_oh);return -1;}
 frame_hash=fnv(frame,(size_t)frame_pitch*frame_h);
 printf("loaded %s: %lu bytes, source %lux%lu, decoded %ux%u pitch %u in %lu ticks, luma at 0x%08lx FNV32=%08lx, prints %ux%u\n",
  APX_SDPRINT_PATH,(unsigned long)bytes,(unsigned long)info.width,(unsigned long)info.height,w,h,frame_pitch,
  (unsigned long)info.ticks,(unsigned long)(uintptr_t)frame,(unsigned long)frame_hash,APX_PRINTER_WIDTH,frame_oh);
 loaded=1;return 0;
}

static int start_print(enum apx_printer_mode m) {
 int e=apx_printer_set_mode(m);
 uint32_t prepare_ticks=0;
 /* Pack the luma (pitch may exceed the width) and run the stock stages on the copy. */
 for(unsigned y=0;y<frame_h&&!e;++y)memcpy(work+(size_t)y*frame_w,frame+(size_t)y*frame_pitch,frame_w);
#if APX_SDPRINT_PREPARE
 if(!e){uint32_t t=apx_ticks();e=apx_photo_prepare(m,work,frame_w,frame_h,scratch,sizeof(scratch));prepare_ticks=apx_ticks()-t;
  if(e)printf("photo prepare error %d\n",e);
  else printf("prepared luma FNV32=%08lx\n",(unsigned long)fnv(work,(size_t)frame_w*frame_h));}
#endif
 if(!e)e=apx_printer_submit_photo_luma(work,(size_t)frame_w*frame_h,frame_w,frame_h,frame_w,APX_PRINTER_WIDTH,frame_oh);
 {struct apx_printer_info p;apx_printer_get_info(&p);
  printf("%s print start %d at %lu%% energy, stock stages %s (%lu ms) (FNV32=%08lx)\n",m==APX_PRINTER_DOT?"dot":"gray",e,(unsigned long)p.energy_percent,
   APX_SDPRINT_PREPARE?"on":"off",(unsigned long)((uint64_t)prepare_ticks*1000/32768),(unsigned long)frame_hash);}
 printing=!e;print_started=apx_ticks();return e;
}

int main(void) {
 apx_usb_init();int power=apx_power_domain_enable(),battery=apx_battery_init();
 printf("SD print demo: power %d battery %d; PRINT prints %s, MODE toggles, EXIT reloads\n",power,battery,
  APX_PRINTER_SD_BOTH?"gray then dot":"in the selected mode");
 apx_buttons_init();
 (void)load();
 unsigned power_armed=0,power_held=0,print_held=0,mode_edge=0,feed_pending=0;
 uint32_t power_since=0,print_since=0;
 for(;;) {
  apx_debug_poll();apx_buttons_poll();apx_battery_poll();apx_printer_poll();apx_maintenance();
  struct apx_buttons_info keys;apx_buttons_get_info(&keys);
  if(!(keys.state&APX_BUTTON_POWER)){power_armed=1;power_held=0;}
  else if(power_armed){if(!power_held){power_since=apx_ticks();power_held=1;}else if(apx_ticks()-power_since>=65536)apx_bootrom();}
  struct apx_button_event event;
  while(apx_buttons_next(&event)) {
   if(event.pressed&APX_BUTTON_EXIT) {
    apx_printer_cancel();second_pending=print_held=feed_pending=0;
    if(!printing&&!feed_busy)(void)load();
   }
   if(!APX_PRINTER_SD_BOTH&&!printing&&!feed_busy&&(event.pressed&APX_BUTTON_MODE)&&!mode_edge){
    mode=mode==APX_PRINTER_GRAY?APX_PRINTER_DOT:APX_PRINTER_GRAY;printf("mode %s\n",mode==APX_PRINTER_DOT?"dot":"gray");
   }
   if(!printing&&!feed_busy&&(event.pressed&APX_BUTTON_PRINT)){print_held=1;print_since=apx_ticks();}
   if((event.released&APX_BUTTON_PRINT)&&print_held) {
    if(print_held==1&&!printing&&!feed_busy) {
     if(!loaded)printf("no image loaded; put %s on the card and press EXIT\n",APX_SDPRINT_PATH);
     else {
      if(APX_PRINTER_SD_BOTH){second_pending=1;(void)start_print(APX_PRINTER_GRAY);if(!printing)second_pending=0;}
      else (void)start_print(mode);
     }
    }
    print_held=0;
   }
  }
  if(print_held==1&&!printing&&!feed_busy&&apx_ticks()-print_since>=32768){feed_pending=1;print_held=2;}
  if(feed_pending&&!printing&&!feed_busy){
   int e=apx_printer_feed(32);feed_pending=0;feed_busy=!e;if(e)printf("paper feed start %d\n",e);
  }
  if((printing||feed_busy)&&!apx_printer_busy()) {
   struct apx_printer_info p;apx_printer_get_info(&p);
   printf("%s ended state=%lu error=%lu rows=%lu temperature_adc=%lu, %lu ms (%lu steps)\n",feed_busy?"feed":"print",
    (unsigned long)p.state,(unsigned long)p.error,(unsigned long)p.row,(unsigned long)p.temperature_adc,
    (unsigned long)((uint64_t)(apx_ticks()-print_started)*1000/32768),(unsigned long)p.steps);
   if(printing&&p.period_count){
    const unsigned long n=p.period_count;
    printf("half-row timing (%lu rows): period min/avg/max %lu/%lu/%lu us, sample avg/max %lu/%lu us, work avg/max %lu/%lu us\n",n,
     (unsigned long)((uint64_t)p.period_min*1000000/32768),(unsigned long)((uint64_t)p.period_sum*1000000/32768/n),(unsigned long)((uint64_t)p.period_max*1000000/32768),
     (unsigned long)((uint64_t)p.sample_sum*1000000/32768/n),(unsigned long)((uint64_t)p.sample_max*1000000/32768),
     (unsigned long)((uint64_t)p.work_sum*1000000/32768/n),(unsigned long)((uint64_t)p.work_max*1000000/32768));
    printf("period overruns (steps later than the fixed row period): %lu\n",(unsigned long)p.period_overruns);
    printf("period histogram <9/<11/<13/<15/<18/<24/>=24 ms: %lu %lu %lu %lu %lu %lu %lu\n",(unsigned long)p.period_hist[0],(unsigned long)p.period_hist[1],
     (unsigned long)p.period_hist[2],(unsigned long)p.period_hist[3],(unsigned long)p.period_hist[4],(unsigned long)p.period_hist[5],(unsigned long)p.period_hist[6]);
   }
   unsigned was_print=printing;printing=feed_busy=0;
   if(was_print&&second_pending&&p.state==APX_PRINTER_DONE&&loaded&&fnv(frame,(size_t)frame_pitch*frame_h)==frame_hash) {
    second_pending=0;(void)start_print(APX_PRINTER_DOT);
   } else if(second_pending) {
    printf("second (dot) print skipped: first print not DONE or frame changed\n");second_pending=0;
   }
  }
 }
}
