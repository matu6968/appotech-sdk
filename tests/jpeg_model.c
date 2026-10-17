#include <appotech/jpeg.h>
#include <appotech/soc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint32_t regs[65536],ticks,writes,cleaned,invalidated;
static unsigned force_error,encode_padding;
static size_t file_size;
static uint8_t original[65536];
uint32_t apx_ticks(void) {return ticks++;}
void apx_delay_ms(uint32_t n) {ticks+=n*33;}
void apx_maintenance(void) {}
uint32_t apx_spr_read(uint32_t r) {return regs[r];}
int apx_dma_clean(const void *p,size_t n) {assert((uintptr_t)p>=APX_RAM_BASE&&(uintptr_t)p+n<=APX_RAM_END);++cleaned;return 0;}
int apx_dma_invalidate(void *p,size_t n) {assert(!((uintptr_t)p&15)&&!(n&15));++invalidated;return 0;}
void apx_spr_write(uint32_t r,uint32_t v) {
 ++writes;regs[r]=v;
 if(r==0xc000 && !(v&128) && (v&0x60000060)==0x60000060) {
  assert(cleaned&&invalidated&&!(regs[0x4800]&(1u<<22)));
  assert((regs[0xc0c0]&255)==224);assert(regs[0xc014]==regs[0xc010]+224*176);
  assert(regs[0xc04c]==0x349d2412);
  memset((void *)(uintptr_t)regs[0xc010],99,224*176);memset((void *)(uintptr_t)regs[0xc014],128,224*88);
  regs[0xc040]=force_error?0x100:0x10040;
 }
 if(r==0xc0a4&&v==800) {
  assert((regs[0xc000]&0x60000000)==0);assert((regs[0xc014]&0x30000000)==0x30000000);
  assert(((volatile uint16_t *)0x66800)[0]==0x300a); /* EOB. */
  assert(((volatile uint16_t *)0x66800)[0x82]==0xefc0); /* 15-bit AC code, length must not be overwritten. */
  memcpy((void *)(uintptr_t)regs[0xc010],original,file_size);
  memset((void *)(uintptr_t)(regs[0xc010]+file_size),0,encode_padding);
  regs[0xc01c]=regs[0xc010]+file_size+encode_padding;regs[0xc040]=0x20041;
 }
}
int main(int argc,char **argv) {
 assert(argc==2);assert(mmap((void *)APX_RAM_BASE,0x400000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void *)APX_RAM_BASE);
 assert(mmap((void *)0x66000,0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void *)0x66000);
 FILE *f=fopen(argv[1],"rb");assert(f);file_size=fread(original,1,sizeof(original),f);assert(feof(f)&&file_size>1000);fclose(f);
 uint8_t *input=(void *)0x02300000,*output=(void *)0x022e0000;memcpy(input,original,file_size);
 struct apx_jpeg_info i;assert(apx_jpeg_inspect(input,file_size,&i)==0&&i.width==220&&i.height==176);
 unsigned before=writes;assert(apx_jpeg_decode_start(input,file_size,output,59135,220,176)==APX_JPEG_RANGE&&writes==before);
 assert(apx_jpeg_decode_start(input+1,file_size,output,59136,220,176)==APX_JPEG_RANGE);
 assert(apx_jpeg_decode_start(input,file_size,input,59136,220,176)==APX_JPEG_RANGE);
 assert(apx_jpeg_decode_start(input,SIZE_MAX,output,59136,220,176)==APX_JPEG_RANGE);
 assert(apx_jpeg_decode_start(input,file_size,output,59136,220,176)==0);
 before=writes;assert(apx_jpeg_decode_start(input,file_size,output,59136,220,176)==APX_JPEG_BUSY&&writes==before);
 apx_jpeg_poll();apx_jpeg_get_info(&i);assert(i.state==APX_JPEG_DONE&&i.error==0&&output[224]==99);
 force_error=1;assert(apx_jpeg_decode_start(input,file_size,output,59136,220,176)==0);apx_jpeg_poll();apx_jpeg_get_info(&i);assert(i.state==APX_JPEG_ERROR&&i.error==APX_JPEG_HARDWARE);force_error=0;
 assert(apx_jpeg_decode_start(input,file_size,output,59136,220,176)==0);regs[0xc040]=0x10000;ticks+=65537;apx_jpeg_poll();apx_jpeg_get_info(&i);assert(i.error==APX_JPEG_TIMEOUT);
 assert(apx_jpeg_encode_start(output,output+224*176,220,176,224,220,176,8,input,1048576)==0);apx_jpeg_poll();apx_jpeg_get_info(&i);assert(i.state==APX_JPEG_DONE&&i.bytes==file_size);
 for(encode_padding=1;encode_padding<=7;++encode_padding) {
  assert(apx_jpeg_encode_start(output,output+224*176,220,176,224,220,176,8,input,1048576)==0);
  apx_jpeg_poll();apx_jpeg_get_info(&i);assert(i.state==APX_JPEG_DONE&&i.bytes==file_size);
 }
 encode_padding=8;
 assert(apx_jpeg_encode_start(output,output+224*176,220,176,224,220,176,8,input,1048576)==0);
 apx_jpeg_poll();apx_jpeg_get_info(&i);assert(i.state==APX_JPEG_ERROR);
 encode_padding=0;
 assert(apx_jpeg_encode_start(output,output+224*176,220,176,224,200,160,8,input,1048576)==APX_JPEG_RANGE);
 memcpy(input,original,file_size);before=writes;
 for(size_t n=0;n<623;++n)assert(apx_jpeg_inspect(input,n,&i)!=0);
 assert(apx_jpeg_inspect(input,file_size-2,&i)==APX_JPEG_INVALID);
 for(size_t n=2;n<623;++n)if(input[n-1]==255&&input[n]==0xc0){input[n]=0xc2;assert(apx_jpeg_inspect(input,file_size,&i)==APX_JPEG_UNSUPPORTED);input[n]=0xc0;break;}
 assert(writes==before);apx_jpeg_shutdown();assert(regs[0xc000]==0&&(regs[0x8840]&(1u<<31)));
 puts("JPEG model: parser bounds, DMA ownership, packed codes, completion/error/timeout passed");
}
