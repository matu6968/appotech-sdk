#define _GNU_SOURCE
#include <appotech/display.h>
#include <appotech/soc.h>
#include <appotech/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#include <string.h>
static uint32_t regs[65536],ticks,pio,dma_count,pixels;
static unsigned rs,cmd,arg_count,high,pending_byte,x0,x1,y0,y1,x,y,madctl;
static uint8_t args[4];
static uint16_t gram[320][320];
static void pixel(uint16_t color) {
 unsigned w=(madctl&0x20)?320:240,h=(madctl&0x20)?240:320;
 assert(x<w&&y<h&&x0<=x1&&y0<=y1);
 gram[y][x]=color;++pixels;
 if(++x>x1){x=x0;if(++y>y1)y=y0;}
}
static void byte(unsigned v) {
 ++pio;
 if(!rs){cmd=v;arg_count=pending_byte=0;if(cmd==0x2c){x=x0;y=y0;}return;}
 if(cmd==0x2c){if(!pending_byte){high=v;pending_byte=1;}else{pixel((high<<8)|v);pending_byte=0;}return;}
 if(cmd==0x36){madctl=v;return;}
 if(cmd!=0x2a&&cmd!=0x2b)return;
 assert(arg_count<4);args[arg_count++]=v;
 if(arg_count==4){unsigned lo=(args[0]<<8)|args[1],hi=(args[2]<<8)|args[3];if(cmd==0x2a){x0=lo;x1=hi;}else{y0=lo;y1=hi;}}
}
uint32_t apx_ticks(void){return ticks++;}
void apx_maintenance(void){}
uint32_t apx_spr_read(uint32_t r){return r==0xa804?(regs[r]|0x80000000u):regs[r];}
void apx_spr_write(uint32_t r,uint32_t value){
 if(r==0xa858){byte(value&255);return;}
 if(r==0xa804){regs[r]=0;return;}
 regs[r]=value;
 if(r==0xa800&&value==0x13){
  unsigned w=regs[0xa840],h=regs[0xa844],pitch=regs[0xc82c]*4;
  const uint8_t *yp=(void *)(uintptr_t)regs[0xc828],*vu=(void *)(uintptr_t)regs[0xc830];
  assert(w&&h&&!(w&1)&&!(h&1)&&pitch>=w&&!(pitch&31));
  assert(vu!=yp&&rs&&cmd==0x2c);
  for(unsigned row=0;row<h;++row)for(unsigned col=0;col<w;++col){unsigned v=yp[row*pitch+col];pixel(APX_RGB565(v,v,v));}
  regs[0xa804]=0x80000008u;regs[0xc80c]=1;++dma_count;
 }
}
int apx_gpio_write(unsigned group,uint32_t mask,uint32_t bits){if(group==4&&(mask&4))rs=!!(bits&4);return 0;}
int apx_gpio_configure(unsigned g,uint32_t m,enum apx_gpio_direction d,enum apx_gpio_pull p,uint32_t h){(void)g;(void)m;(void)d;(void)p;(void)h;return 0;}
int apx_power_update(unsigned a,uint8_t m,uint8_t v){assert(a==0x54&&m==0x40);(void)v;return 0;}
int apx_dma_clean(const void *p,size_t n){(void)p;(void)n;return 0;}
static void finish(void){for(unsigned i=0;i<200000&&(apx_display_status()==APX_DISPLAY_INITIALIZING||apx_display_status()==APX_DISPLAY_BUSY);++i)apx_display_poll();assert(apx_display_status()==APX_DISPLAY_READY);}
int main(void){
 uint16_t *rgb=mmap((void *)0x02230000,0x40000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);assert(rgb!=MAP_FAILED);
 apx_display_init();finish();assert(regs[0xa80c]==0x44&&regs[0x887c]==56);
 for(unsigned rotation=0;rotation<4;++rotation){
  assert(!apx_display_set_rotation(rotation));unsigned w=apx_display_width(),h=apx_display_height();
  assert(w*h==76800);
  assert(!apx_display_fill(0,0,w,h,0x1234));finish();
  for(unsigned row=0;row<h;++row)for(unsigned col=0;col<w;++col)assert(gram[row][col]==0x1234);
  assert(!apx_display_fill(w-1,h-1,1,1,0xabcd));finish();assert(gram[h-1][w-1]==0xabcd);
  assert(apx_display_fill(w,0,1,1,0)==-1);
  for(unsigned i=0;i<w*h;++i)rgb[i]=APX_RGB565(128,128,128);
  unsigned before=dma_count;assert(!apx_display_blit_dma(0,0,w,h,rgb));finish();
  assert(dma_count-before>=2);assert(gram[h-1][w-1]!=0x1234);
 }
 assert(pio&&pixels==4*(76800+1+76800));
 apx_display_shutdown();puts("ST7789 model: stock init, all rotations, edge windows, full PIO and multi-strip DMA passed");
}
