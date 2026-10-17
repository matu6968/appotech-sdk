#include <appotech/camera_files.h>
#include <appotech/camera.h>
#include <appotech/jpeg.h>
#include <appotech/display.h>
#include <appotech/storage.h>
#include <appotech/debug.h>
#include <appotech/soc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct apx_camera_info camera;
static struct apx_jpeg_info jpeg;
static uint8_t *jpeg_buffer;
static unsigned complete_camera,complete_jpeg,display_count,file_open,file_exists,written,sync_error;
static uint32_t ticks;
static int init_result;
static unsigned pattern_frames;
static enum apx_display_state display_state=APX_DISPLAY_READY;
static const uint8_t *display_buffer;
static uint8_t out[4096];static size_t out_bytes;
uint32_t apx_ticks(void) {return ticks+=100;}
void apx_maintenance(void) {}
void apx_usb_poll(void) {}
void apx_buttons_poll(void) {}
void apx_display_poll(void) {}
uint32_t apx_spr_read(uint32_t reg) {(void)reg;return 0;}
int apx_dma_clean(const void *p,size_t n) {(void)p;(void)n;return 0;}
int apx_dma_invalidate(void *p,size_t n) {(void)p;(void)n;return 0;}
int apx_camera_init(void) {camera.state=init_result?APX_CAMERA_ERROR:APX_CAMERA_READY;camera.error=init_result;return init_result;}
void apx_camera_get_info(struct apx_camera_info *i) {*i=camera;}
void apx_camera_stop(void) {camera.state=APX_CAMERA_READY;}
int apx_camera_capture_start(void *p,size_t capacity) {
 if(init_result)return init_result; /* the real call re-probes the sensor */
 assert(capacity==460800&&!((uintptr_t)p&31));camera.state=APX_CAMERA_CAPTURING;camera.buffer=(uintptr_t)p;
 camera.scaled=0;return 0;
}
int apx_camera_scaled_start(void *p,size_t capacity,unsigned w,unsigned h,unsigned pitch) {
 assert(capacity==59136&&w==220&&h==176&&pitch==224&&!((uintptr_t)p&31));
 assert(display_state!=APX_DISPLAY_BUSY||p!=display_buffer);
 camera.state=APX_CAMERA_CAPTURING;camera.buffer=(uintptr_t)p;camera.scaled=1;
 camera.width=w;camera.height=h;camera.pitch=pitch;return 0;
}
void apx_camera_poll(void) {if(complete_camera&&camera.state==APX_CAMERA_CAPTURING){
 uint8_t *p=(void *)(uintptr_t)camera.buffer;
 if(camera.scaled){for(unsigned n=0;n<59136;++n)p[n]=(n/224*5+n%224*3)&255;}
 else {for(unsigned n=0;n<307200;++n)p[n]=(n/640*5+n%640*3)&255;
 for(unsigned n=0;n<153600;++n)p[307200+n]=(n/640*7+n%640)&255;}
 camera.state=APX_CAMERA_DONE;++camera.completed;complete_camera=0;}}
enum apx_display_state apx_display_status(void) {return display_state;}
int apx_display_blit_nv21(unsigned x,unsigned y,unsigned w,unsigned h,const uint8_t *luma,const uint8_t *chroma,unsigned pitch) {
 assert(!x&&!y&&w==220&&h==176&&pitch==224&&chroma==luma+224*176);
 if(init_result){ /* sensor-less test pattern: eight vertical bars, white to black */
  assert(display_state==APX_DISPLAY_READY&&luma[0]==235&&luma[100*224+223]==16);
  for(unsigned row=0;row<176;++row)assert(luma[row*224+5]==235&&luma[row*224+30]==210&&luma[row*224+60]==170&&luma[row*224+100]==145);
  assert(chroma[0]==128&&chroma[30]==146&&chroma[31]==16&&chroma[87*224+30]==146&&chroma[87*224+31]==16);
  display_buffer=luma;display_state=APX_DISPLAY_BUSY;++display_count;++pattern_frames;return 0;
 }
 assert(display_state==APX_DISPLAY_READY&&luma[0]==0&&luma[224*175+219]==((175*5+219*3)&255));
 assert(chroma[224*87+219]==(((176+87)*5+219*3)&255));
 display_buffer=luma;display_state=APX_DISPLAY_BUSY;++display_count;return 0;
}
void apx_jpeg_poll(void) {if(complete_jpeg&&jpeg.state==APX_JPEG_ENCODING){
 memset(jpeg_buffer,0x23,32);jpeg.bytes=32;jpeg.state=APX_JPEG_DONE;complete_jpeg=0;}}
void apx_jpeg_get_info(struct apx_jpeg_info *i) {*i=jpeg;}
void apx_jpeg_shutdown(void) {jpeg.state=APX_JPEG_IDLE;}
int apx_jpeg_encode_start(const void *y,const void *vu,unsigned sw,unsigned sh,unsigned pitch,
                         unsigned w,unsigned h,unsigned selector,void *buffer,size_t capacity) {
 assert(y==(void *)(uintptr_t)camera.buffer&&vu==(uint8_t *)y+pitch*sh&&pitch==((sw+31)&~31u));
 assert(selector==8&&capacity==524288&&buffer!=y);
 if(w<640){assert(sw==w&&sh==h);
  const uint8_t *l=y,*c=vu;
  for(unsigned row=0;row<h;++row)for(unsigned col=0;col<w;++col)
   assert(l[row*pitch+col]==((row*480/h*5+col*640/w*3)&255));
  for(unsigned row=0;row<h/2;++row)for(unsigned col=0;col<w;col+=2){
   unsigned offset=(col*640/w)&~1u,source_row=(row*2*480/h)/2;
   assert(c[row*pitch+col]==((source_row*7+offset)&255)&&c[row*pitch+col+1]==((source_row*7+offset+1)&255));}
 }else assert(sw==640&&sh==480);
 jpeg_buffer=buffer;jpeg.state=APX_JPEG_ENCODING;
 return 0;
}
FRESULT apx_storage_mount(void) {return FR_OK;}
FRESULT f_open(FIL *f,const TCHAR *path,BYTE mode) {
 (void)f;assert(!strcmp(path,"0:/photo.jpg")&&mode==(FA_WRITE|FA_CREATE_NEW));
 if(file_exists)return FR_EXIST;
 file_open=1;written=0;return FR_OK;
}
FRESULT f_write(FIL *f,const void *buffer,UINT n,UINT *count) {
 (void)f;assert(file_open&&buffer==jpeg_buffer&&n==32);written+=n;*count=n;return FR_OK;
}
FRESULT f_sync(FIL *f) {(void)f;assert(file_open);return sync_error?FR_DISK_ERR:FR_OK;}
FRESULT f_close(FIL *f) {(void)f;assert(file_open);file_open=0;file_exists=1;return FR_OK;}
static void put32(uint8_t *p,uint32_t v) {for(unsigned n=0;n<4;++n)p[n]=v>>(8*n);}
static uint32_t command(unsigned cmd,const void *in,size_t n) {return apx_camera_command(cmd,in,n,out,&out_bytes);}
static void job(unsigned n) {char text[32];snprintf(text,sizeof(text),"\"job\":%u",n);assert(!command(0,NULL,0)&&strstr((char *)out,text));}
static void sensorless(void) {
 /* Disconnected sensor: the init error is reported, yet the preview shows bars. */
 for(int e=APX_CAMERA_NACK;e<=APX_CAMERA_NACK;++e){
  init_result=e;display_state=APX_DISPLAY_READY;pattern_frames=0;
  assert(apx_camera_files_init()==e);job(1);
  ticks=100000;apx_camera_files_poll();assert(pattern_frames==1&&display_state==APX_DISPLAY_BUSY);
  apx_camera_files_poll();assert(pattern_frames==1); /* display still busy: no second blit */
  display_state=APX_DISPLAY_READY;apx_camera_files_poll();assert(pattern_frames==1); /* rate limited to ~10 Hz */
  ticks+=4000;apx_camera_files_poll();assert(pattern_frames==2);
  display_state=APX_DISPLAY_READY;
  /* A photo cannot be taken without a sensor, but a failed capture must not wedge the service. */
  uint8_t shot[12];put32(shot,220);put32(shot+4,176);put32(shot+8,8);
  assert(command(2,shot,12)!=0);job(5);
  uint8_t on[4]={1,0,0,0};assert(!command(1,on,4));job(1);
  ticks+=4000;apx_camera_files_poll();assert(pattern_frames==3);
 }
 init_result=APX_CAMERA_TIMEOUT; /* other failures stay failures */
 assert(apx_camera_files_init()==APX_CAMERA_TIMEOUT);job(5);
 init_result=APX_CAMERA_UNSUPPORTED;display_state=APX_DISPLAY_READY;
 assert(apx_camera_files_init()==APX_CAMERA_UNSUPPORTED);job(1);
 init_result=0;
}
int main(void) {
 assert(!apx_camera_files_init());ticks=2000;apx_camera_files_poll();assert(camera.state==APX_CAMERA_CAPTURING);
 complete_camera=1;apx_camera_files_poll();assert(display_count==1);
 const uint8_t *first=display_buffer;assert((void *)(uintptr_t)camera.buffer!=first&&camera.state==APX_CAMERA_CAPTURING);
 complete_camera=1;apx_camera_files_poll();assert(display_count==1&&camera.state==APX_CAMERA_DONE);
 for(unsigned n=0;n<10;++n)apx_camera_files_poll();
 assert(display_count==1&&camera.state==APX_CAMERA_DONE&&display_buffer==first);
 display_state=APX_DISPLAY_READY;apx_camera_files_poll();
 assert(display_count==2&&display_buffer!=first&&(void *)(uintptr_t)camera.buffer==first&&camera.state==APX_CAMERA_CAPTURING);
 complete_camera=1;((uint8_t *)first)[-1]=0;apx_camera_files_poll();job(5);assert(camera.state==APX_CAMERA_READY);
 ((uint8_t *)first)[-1]=0xa5;display_state=APX_DISPLAY_READY;
 uint8_t enabled[4]={1,0,0,0};assert(!command(1,enabled,4));apx_camera_files_poll();
 display_state=APX_DISPLAY_ERROR;apx_camera_files_poll();job(5);assert(camera.state==APX_CAMERA_READY);
 display_state=APX_DISPLAY_READY;
 assert(!command(5,NULL,0));job(0);
 uint8_t payload[12]={0};assert(command(1,payload,3)==APXD_INVALID);
 put32(payload,200);put32(payload+4,176);put32(payload+8,8);assert(command(2,payload,12)==0x502);
 assert(!command(6,NULL,0));job(6);apx_camera_files_poll();job(6);
 assert(command(7,payload,8)==0x503);assert(command(6,NULL,0)==0x503);
 complete_camera=1;apx_camera_files_poll();job(7);
 put32(payload,460799);put32(payload+4,1);assert(!command(7,payload,8)&&out_bytes==1&&out[0]==((239*7+639)&255));
 put32(payload,0xfffffff0);put32(payload+4,4096);assert(command(7,payload,8)==0x502);
 uint8_t *frame=(uint8_t *)(uintptr_t)camera.buffer;frame[-1]=0;
 put32(payload,0);put32(payload+4,1);assert(command(7,payload,8)==0x505);frame[-1]=0xa5;
 put32(payload,2048);put32(payload+4,1536);put32(payload+8,8);assert(!command(2,payload,12));
 complete_camera=1;apx_camera_files_poll();job(3);
 assert(command(1,payload,4)==APXD_INVALID);complete_jpeg=1;apx_camera_files_poll();job(4);
 put32(payload,0);put32(payload+4,32);assert(!command(3,payload,8)&&out_bytes==32&&out[31]==0x23);
 const char path[]="0:/photo.jpg";sync_error=1;assert(command(4,path,sizeof(path))==0x101&&written==32&&!file_open);
 assert(command(4,path,sizeof(path))==0x108);file_exists=sync_error=0;assert(!command(4,path,sizeof(path))&&!file_open);
 assert(command(4,"x\0tail",7)==APXD_INVALID);
 assert(!command(5,NULL,0));job(0);assert(command(3,payload,8)==0x503);
 for(unsigned trial=0;trial<2;++trial){
  put32(payload,trial?320:220);put32(payload+4,trial?240:176);put32(payload+8,8);
  assert(!command(2,payload,12));complete_camera=1;apx_camera_files_poll();job(3);
  complete_jpeg=1;apx_camera_files_poll();job(4);assert(!command(5,NULL,0));
 }
 sensorless();
 puts("Camera service model: scaler preview, two-buffer backpressure, LCD ownership, guard abort, raw/JPEG bounds, guards, busy/cancel and create-new/sync/close passed");
}
