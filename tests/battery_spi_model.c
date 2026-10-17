#include <appotech/adc.h>
#include <appotech/battery.h>
#include <appotech/spi.h>
#include <appotech/buttons.h>
#include <appotech/debug.h>
#include <appotech/soc.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static uint32_t regs[65536],now,adc_done,calibration=803;
static unsigned channel,adc_stuck,spi_stuck,bat_adc=750,ref_adc=300,button_adc=1023;
static unsigned stale_model,fresh_conversions,last_result,conversions;
static uint8_t flash[APX_SPI_CAPACITY],command,status_reg;
static unsigned position,selected,spi_address,write_count,bytes_tx;
static uint8_t spi_byte(uint8_t value,unsigned receive){
 assert(selected);++bytes_tx;
 if(!position++){command=value;spi_address=write_count=0;if(command==6)status_reg|=2;return 0;}
 if(command==0x9f){static const uint8_t id[]={0x5e,0x40,0x15};assert(receive&&position<=4);return id[position-2];}
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
uint32_t apx_mmio_read32(uintptr_t address){assert(address==0x1fec);return calibration;}
uint32_t apx_spr_read(uint32_t r){
 if(r==0x9170&&(regs[r]&0x40)&&!adc_stuck&&(int32_t)(now-adc_done)>=0)regs[r]&=~0x40u;
 if(r==0x9178){
  unsigned fresh=channel==9?bat_adc:channel==11?ref_adc:button_adc;
  /* Observed cold-boot hardware: the first conversion after a channel change still
   * carries the previously converted channel (reference read as the battery level). */
  unsigned value=stale_model&&!fresh_conversions?last_result:fresh;
  return value;
 }
 return regs[r];
}
void apx_spr_write(uint32_t r,uint32_t value){
 uint32_t old=regs[r];regs[r]=value;
 if(r==0x9170&&(value&0x80)){assert(!(old&0x40));
  if(channel!=(value&15u)||!conversions)fresh_conversions=0;else ++fresh_conversions;
  last_result=channel==9?bat_adc:channel==11?ref_adc:button_adc;++conversions;
  channel=value&15;regs[r]=(value&~0x80u)|0x40;adc_done=now+4;}
 if(r==0x9000){
  if((old&0x4000)&&!(value&0x4000)){selected=1;position=0;}
  if(!(old&0x4000)&&(value&0x4000)&&selected){
   if(command==0x20&&position==4){assert(status_reg&2);memset(flash+spi_address,255,4096);status_reg&=~2u;}
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
static void run(unsigned ticks){for(unsigned i=0;i<ticks;i+=4){now+=4;apx_buttons_poll();apx_battery_poll();}}
static struct apx_battery_info battery(void){struct apx_battery_info i;apx_battery_get_info(&i);return i;}
int main(void){
 calibration=0;assert(apx_battery_init()==-1);calibration=803;
 now=0xffff0000;regs[0x9170]=0x13;regs[0x8840]=1u<<29;regs[0x88e0]=1u<<21;
 regs[0x8868]=0x044d582e;apx_buttons_init();assert(!apx_battery_init());assert(regs[0x8868]==0x044dd82e);run(32768);
 assert(battery().valid&&battery().millivolts==4034&&battery().level==4&&battery().external_power);
 button_adc=484;run(1000);struct apx_buttons_info b;apx_buttons_get_info(&b);assert(b.state==APX_BUTTON_UP);
 /* Remain at level4 until crossing 4060+30; then require <4060-30 to fall. */
 bat_adc=760;run(32768);assert(battery().level==4);
 bat_adc=770;run(32768);assert(battery().level==5);
 bat_adc=755;run(32768);assert(battery().level==5);
 bat_adc=740;run(32768);assert(battery().level==4);
 ref_adc=0;run(32768);assert(!battery().valid&&battery().invalid_samples);
 ref_adc=300;adc_stuck=1;run(32768);assert(!battery().valid&&battery().timeouts);
 adc_stuck=0;run(32768);assert(battery().valid);
 regs[0x88e0]=0;run(1000);assert(!battery().external_power);
 /* Stale-first-conversion hardware: without a discarded conversion channel 11 would
  * read like channel 9 (reference ~ battery) and the voltage would fall out of range. */
 stale_model=1;bat_adc=750;ref_adc=300;run(65536);
 assert(battery().valid&&battery().millivolts==4034&&battery().adc_battery==750&&battery().adc_reference==300);
 stale_model=0;
 apx_buttons_shutdown();run(32768);assert(battery().valid);apx_battery_shutdown();assert(regs[0x8868]==0x044d582e); /* reference bit restored */
 assert(regs[0x9170]==0x13&&(regs[0x8840]&(1u<<29)));
 assert(apx_adc_begin(APX_ADC_BUTTONS,3)==-1);
 for(unsigned i=0;i<sizeof(flash);++i)flash[i]=(i*37u+(i>>8))&255;
 assert(!apx_spi_init());struct apx_spi_info info;apx_spi_get_info(&info);assert(info.jedec==0x5e4015);
 uint8_t out[8192];const unsigned sizes[]={0,1,7,15,16,31,32,33,63,64,65,255,256,511,512,513,4095,4096,4097,8192};
 for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i){assert(!apx_spi_read(17,out,sizes[i]));assert(!memcmp(out,flash+17,sizes[i]));}
 assert(!apx_spi_read(APX_SPI_CAPACITY,0,0));assert(apx_spi_read(APX_SPI_CAPACITY,out,1)==APX_SPI_INVALID);
 assert(apx_spi_read(0xffffffff,out,1)==APX_SPI_INVALID);assert(apx_spi_read(0,0,1)==APX_SPI_INVALID);
 unsigned tx_before=bytes_tx;assert(apx_spi_program(255,out,2)==APX_SPI_INVALID);assert(apx_spi_erase_sector(1)==APX_SPI_INVALID);assert(bytes_tx==tx_before);
 memset(out,0x55,256);assert(!apx_spi_erase_sector(4096));assert(flash[4096]==255&&flash[8191]==255);
 assert(!apx_spi_program(4096,out,256));assert(!memcmp(flash+4096,out,256));
 memset(out,0xaa,256);assert(!apx_spi_program(4096,out,256));for(unsigned i=0;i<256;++i)assert(!flash[4096+i]);
 spi_stuck=1;assert(apx_spi_read(0,out,1)==APX_SPI_TIMEOUT&&!selected);spi_stuck=0;
 assert(!apx_spi_read(0,out,64));apx_spi_get_info(&info);assert(info.timeouts==1&&info.dma_transfers);
 uint8_t in[8]={0};size_t n;in[4]=33;assert(!apx_spi_command(1,in,8,out,&n)&&n==33);
 assert(apx_spi_command(1,in,7,out,&n)==APXD_INVALID);in[6]=1;assert(apx_spi_command(1,in,8,out,&n)==APXD_RANGE);
 assert(apx_spi_command(2,in,0,out,&n)==APXD_INVALID);
 uint8_t write[264]={'A','P','X','F',0,0x20,0,0};memset(write+8,0x31,256);
 assert(!apx_spi_command(3,write,8,out,&n)&&!n);
 assert(!apx_spi_command(2,write,sizeof(write),out,&n)&&!n);
 assert(!memcmp(flash+8192,write+8,256));
 tx_before=bytes_tx;write[0]='B';assert(apx_spi_command(3,write,8,out,&n)==APXD_INVALID);
 write[0]='A';write[4]=1;assert(apx_spi_command(3,write,8,out,&n)==APXD_RANGE);
 assert(apx_spi_command(2,write,sizeof(write),out,&n)==APXD_RANGE&&bytes_tx==tx_before);
 apx_spi_shutdown();assert(apx_spi_command(0,in,0,out,&n)==APXD_UNSUPPORTED);
 puts("Battery/SPI model passed: ADC arbitration, calibration, filter/hysteresis, supply, failures, wrap, DMA boundaries, NOR programming/erase, range rejection, teardown");
}
