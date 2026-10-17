#include <appotech/uart.h>
#include <appotech/debug.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
static uint32_t regs[65536],now;
static uint8_t transmitted[8192];static unsigned count,stuck;
static unsigned auto_tick;
uint32_t apx_ticks(void){return auto_tick?++now:now;}
uint32_t apx_spr_read(uint32_t r){return regs[r];}
void apx_spr_write(uint32_t r,uint32_t value){
 if(r==0x908c){if(value&1)regs[r]&=~2u;if(value&0x10)regs[r]&=~1u;return;}
 regs[r]=value;
 if(r==0x9088){assert(count<sizeof(transmitted));transmitted[count++]=value;if(!stuck)regs[0x908c]|=2;}
}
int main(void){
 uint8_t buffer[600];memset(buffer,0x75,sizeof(buffer));
 assert(apx_uart_write(buffer,1)==-1);assert(apx_uart_init(0,6,-1)==-1);assert(apx_uart_init(115200,32,-1)==-1);
 for(unsigned i=0;i<65536;++i)regs[i]=0xa5a555aa;
 uint32_t saved[65536];memcpy(saved,regs,sizeof(saved));assert(!apx_uart_init(115200,6,1));
 struct apx_uart_info info;apx_uart_get_info(&info);assert(info.initialized&&info.baud==APX_CPU_HZ_ESTIMATE/((APX_CPU_HZ_ESTIMATE+57600)/115200)&&info.divider==(APX_CPU_HZ_ESTIMATE+57600)/115200-1&&info.rx_route==1);
 assert((regs[0x8340]&1023)==(6u<<5|1));assert(!(regs[0x8040]&0x80)&&(regs[0x8100]&0x80));
 assert(!apx_uart_write(buffer,512));assert(apx_uart_write(buffer,2049)==-1);
 for(unsigned i=0;i<600;++i){now+=4;apx_uart_poll();}
 apx_uart_get_info(&info);assert(info.tx_bytes==512&&!info.tx_queued&&!info.timeouts&&count==512&&!memcmp(buffer,transmitted,512));
 for(unsigned i=0;i<530;++i){regs[0x9088]=i;regs[0x908c]|=1;apx_uart_poll();}
 apx_uart_get_info(&info);assert(info.rx_bytes==530&&info.rx_dropped==18&&info.rx_queued==512);
 assert(apx_uart_read(buffer,600)==512);for(unsigned i=0;i<512;++i)assert(buffer[i]==(i&255));
 stuck=1;regs[0x908c]&=~2u;assert(!apx_uart_write(buffer,1));now+=329;apx_uart_poll();apx_uart_get_info(&info);assert(info.timeouts==1&&!info.tx_queued);
 /* console mirror: bare '\n' becomes CR LF, an existing CR LF is left alone, and a burst larger than the queue is drained, not dropped */
 stuck=0;regs[0x908c]|=2;now+=1000;apx_uart_poll();count=0;
 assert(!apx_uart_console_enable(1));
 for(const char *t="a\nb\r\nc";*t;++t){apx_uart_console_put(*t);for(unsigned i=0;i<4;++i){now+=4;apx_uart_poll();}}
 for(unsigned i=0;i<64;++i){now+=4;apx_uart_poll();}
 {static const char want[]="a\r\nb\r\nc";assert(!memcmp(transmitted,want,sizeof(want)-1));assert(count==sizeof(want)-1);}
 auto_tick=1;for(unsigned i=0;i<5000;++i)apx_uart_console_put('x');auto_tick=0;
 stuck=0;regs[0x908c]|=2;for(unsigned i=0;i<6000;++i){now+=4;apx_uart_poll();}
 apx_uart_get_info(&info);assert(info.console_enabled&&info.console_dropped==0);
 stuck=1;regs[0x908c]&=~2u;auto_tick=1;for(unsigned i=0;i<5000;++i)apx_uart_console_put('y');auto_tick=0;
 apx_uart_get_info(&info);assert(info.timeouts>=1);
 apx_uart_shutdown();for(unsigned i=0;i<6;++i){static const unsigned r[]={0x8340,0x8000,0x8040,0x8080,0x80c0,0x8100};assert(regs[r[i]]==saved[r[i]]);}
 assert(regs[0x9080]==saved[0x9080]&&regs[0x9084]==saved[0x9084]);
 uint8_t out[4096],input[12]={0};size_t n;
 assert(apx_uart_command(1,input,11,out,&n)==APXD_INVALID);assert(apx_uart_command(1,input,12,out,&n)==APXD_RANGE);
 assert(!apx_uart_command(0,input,0,out,&n)&&n&&strstr((char *)out,"8N1"));
 regs[0x9084]=0;assert(!apx_uart_init(115200,6,-1));apx_uart_shutdown();assert(regs[0x9084]==1249);
 puts("UART model passed: explicit routing, 8N1 divider, binary queues, receive overflow, timeout, field-preserving shutdown and command validation");
}
