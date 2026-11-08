/* SPI NOR driver (src/spi.c) against an independent controller + flash model, built per board.
 * The board header supplies the JEDEC ID and size (Action 5e4015 / 2 MiB, Silvergear 944016 / 4 MiB), so the same test
 * proves that the driver accepts exactly its board's chip, reaches every byte up to the last sector, and rejects the
 * first address past the end. The model is a bit-level host check of the driver; it is not hardware evidence. */
#include <appotech/spi.h>
#include <appotech/debug.h>
#include <appotech/soc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static uint32_t regs[65536],now;
static uint8_t flash[APX_SPI_CAPACITY],command,status_reg;
static uint8_t jedec_id[3]={APX_SPI_JEDEC>>16,(APX_SPI_JEDEC>>8)&255,APX_SPI_JEDEC&255};
static unsigned position,selected,spi_address,write_count,bytes_tx,erases,spi_stuck;
static uint8_t spi_byte(uint8_t value,unsigned receive){
 assert(selected);++bytes_tx;
 if(!position++){command=value;spi_address=write_count=0;if(command==6)status_reg|=2;return 0;}
 if(command==0x9f){assert(receive&&position<=4);return jedec_id[position-2];}
 if(command==5)return status_reg;
 if(command==3||command==2||command==0x20){
  if(position<=4){spi_address=(spi_address<<8)|value;return 0;}
  assert(spi_address<APX_SPI_CAPACITY);
  if(command==3){assert(receive);return flash[spi_address++];}
  if(command==2){assert(status_reg&2);flash[spi_address++]&=value;++write_count;return 0;}
 }
 return 0xff;
}
uint32_t apx_ticks(void){return now;}
void apx_maintenance(void){now+=4;}
uint32_t apx_spr_read(uint32_t r){return regs[r];}
void apx_spr_write(uint32_t r,uint32_t value){
 uint32_t old=regs[r];regs[r]=value;
 if(r==0x9000){
  if((old&0x4000)&&!(value&0x4000)){selected=1;position=0;}
  if(!(old&0x4000)&&(value&0x4000)&&selected){
   if(command==0x20&&position==4){assert(status_reg&2);memset(flash+(spi_address&~4095u),255,4096);status_reg&=~2u;++erases;}
   if(command==2&&write_count)status_reg&=~2u;
   selected=0;}
 }
 if(r==0x9244&&value==1){regs[0x9248]&=~1u;if(!spi_stuck){regs[0x9250]=spi_byte(regs[0x9250],regs[0x9000]&0x80);regs[0x9248]|=1;}}
 if(r==0x9244&&value==0x20000)regs[0x9248]&=~2u;
 if(r==0x9244&&value==2&&!spi_stuck){
  assert(regs[0x900c]&0x80);uint8_t *p=(void *)(uintptr_t)regs[0x9258];
  unsigned n=(regs[0x9254]+1)*4;assert(n&&n<=4096&&!(n%32));
  for(unsigned i=0;i<n;++i)p[i]=spi_byte(255,1);
  regs[0x9248]|=2;
 }
}
int apx_dma_clean(const void *p,size_t n){assert(p&&n);return 0;}
int apx_dma_invalidate(void *p,size_t n){assert(p&&n&&!((uintptr_t)p%32)&&!(n%32));return 0;}
int main(void){
 for(size_t i=0;i<sizeof(flash);++i)flash[i]=(i*37u+(i>>8)+(i>>20))&255;
 now=0xffff0000;
 /* A chip with another ID is refused and leaves the driver uninitialised. */
 uint8_t other[3]={0xef,0x40,0x16};memcpy(jedec_id,other,3);
 assert(apx_spi_init()==APX_SPI_HARDWARE);struct apx_spi_info info;apx_spi_get_info(&info);assert(!info.initialized);
 const uint8_t ours[3]={APX_SPI_JEDEC>>16,(APX_SPI_JEDEC>>8)&255,APX_SPI_JEDEC&255};memcpy(jedec_id,ours,3);
 assert(!apx_spi_init());apx_spi_get_info(&info);assert(info.jedec==APX_SPI_JEDEC&&info.initialized);
 static uint8_t out[8192];
 const unsigned sizes[]={0,1,7,31,32,33,255,256,513,4095,4096,4097,8192};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i){assert(!apx_spi_read(17,out,sizes[i]));assert(!memcmp(out,flash+17,sizes[i]));}
 /* Reads reach the last byte; the first address past the end is refused. */
 assert(!apx_spi_read(APX_SPI_CAPACITY-1,out,1)&&out[0]==flash[APX_SPI_CAPACITY-1]);
 assert(!apx_spi_read(APX_SPI_CAPACITY-8192,out,8192)&&!memcmp(out,flash+APX_SPI_CAPACITY-8192,8192));
 assert(!apx_spi_read(APX_SPI_CAPACITY,0,0));assert(apx_spi_read(APX_SPI_CAPACITY,out,1)==APX_SPI_INVALID);
 assert(apx_spi_read(APX_SPI_CAPACITY-1,out,2)==APX_SPI_INVALID);
 assert(apx_spi_read(0xffffffff,out,1)==APX_SPI_INVALID);assert(apx_spi_read(0,0,1)==APX_SPI_INVALID);
#if APX_SPI_CAPACITY>0x200000u
 /* Addresses above 2 MiB use the third address byte's upper bits (a 2 MiB-only driver would wrap or refuse). */
 assert(!apx_spi_read(0x200000,out,4096)&&!memcmp(out,flash+0x200000,4096));
 assert(!apx_spi_read(0x1ffffe,out,4)&&!memcmp(out,flash+0x1ffffe,4));
#endif
 unsigned tx_before=bytes_tx;
 assert(apx_spi_program(255,out,2)==APX_SPI_INVALID);assert(apx_spi_erase_sector(1)==APX_SPI_INVALID);
 assert(apx_spi_erase_sector(APX_SPI_CAPACITY)==APX_SPI_INVALID);assert(apx_spi_program(APX_SPI_CAPACITY,out,1)==APX_SPI_INVALID);
 assert(bytes_tx==tx_before);
 /* Erase/program the last sector and the first writable one. */
 const uint32_t last=APX_SPI_CAPACITY-4096;
 memset(out,0x55,256);assert(!apx_spi_erase_sector(last)&&erases==1);assert(flash[last]==255&&flash[APX_SPI_CAPACITY-1]==255);
 assert(!apx_spi_program(last+4096-256,out,256));assert(!memcmp(flash+APX_SPI_CAPACITY-256,out,256));
 assert(flash[last-1]==(uint8_t)((last-1)*37u+((last-1)>>8)+((last-1)>>20))); /* neighbour untouched */
 assert(!apx_spi_erase_sector(4096)&&erases==2);assert(!apx_spi_program(4096,out,256)&&!memcmp(flash+4096,out,256));
 memset(out,0xaa,256);assert(!apx_spi_program(4096,out,256));for(unsigned i=0;i<256;++i)assert(!flash[4096+i]);
 spi_stuck=1;assert(apx_spi_read(0,out,1)==APX_SPI_TIMEOUT&&!selected);spi_stuck=0;
 assert(!apx_spi_read(0,out,64));apx_spi_get_info(&info);assert(info.timeouts==1&&info.dma_transfers);
 /* APXD: the status JSON reports this board's capacity; ranges follow it. */
 uint8_t in[8]={0};size_t n;assert(!apx_spi_command(0,in,0,out,&n)&&n);out[n]=0;
 char expected[40];snprintf(expected,sizeof(expected),"\"capacity\":%lu,",(unsigned long)APX_SPI_CAPACITY);
 assert(strstr((char *)out,expected));
 snprintf(expected,sizeof(expected),"\"jedec\":%lu,",(unsigned long)APX_SPI_JEDEC);assert(strstr((char *)out,expected));
 in[4]=33;assert(!apx_spi_command(1,in,8,out,&n)&&n==33);
 uint32_t end=APX_SPI_CAPACITY;memcpy(in,&end,4);in[4]=1;in[5]=0;assert(apx_spi_command(1,in,8,out,&n)==APXD_RANGE);
 end=APX_SPI_CAPACITY-1;memcpy(in,&end,4);in[4]=1;assert(!apx_spi_command(1,in,8,out,&n)&&n==1);
 uint8_t write[264]={'A','P','X','F'};uint32_t sector=last;memcpy(write+4,&sector,4);memset(write+8,0x31,256);
 assert(!apx_spi_command(3,write,8,out,&n)&&!n);
 sector=last+4096-256;memcpy(write+4,&sector,4);assert(!apx_spi_command(2,write,sizeof(write),out,&n)&&!n);
 assert(!memcmp(flash+APX_SPI_CAPACITY-256,write+8,256));
 tx_before=bytes_tx;sector=APX_SPI_CAPACITY;memcpy(write+4,&sector,4);
 assert(apx_spi_command(3,write,8,out,&n)==APXD_RANGE);assert(apx_spi_command(2,write,sizeof(write),out,&n)==APXD_RANGE);
 write[0]='B';sector=last;memcpy(write+4,&sector,4);assert(apx_spi_command(3,write,8,out,&n)==APXD_INVALID);
 assert(bytes_tx==tx_before);
 apx_spi_shutdown();assert(apx_spi_command(0,in,0,out,&n)==APXD_UNSUPPORTED);
 printf("SPI model passed: %s, JEDEC %06lx, %lu KiB: ID check, full-range reads, last-sector erase/program, APXD ranges\n",
        APX_BOARD_ID,(unsigned long)APX_SPI_JEDEC,(unsigned long)(APX_SPI_CAPACITY>>10));
}
