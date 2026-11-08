#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
void model_reset(void),model_set_flash(uint32_t),model_set_flash_status(unsigned);
void model_set_sram_window(const void *,uint64_t),model_call(void *);
uint8_t *model_nor(void),*model_pm(void);
uint32_t model_command_count(unsigned);
static struct {uint32_t magic,seq,cmd,address,length,status,flags;uint8_t data[512];} p;
static uint8_t before[0x400000];
static unsigned call(unsigned cmd,unsigned address,unsigned length,unsigned flags) {
 memset(&p,0,sizeof p);p.magic=0x52585041;p.seq=1;p.cmd=cmd;p.address=address;p.length=length;
 p.status=0xffffffff;p.flags=flags;memset(p.data,0x42,length);model_call(&p);return p.status;
}
static void reset(unsigned id) {model_reset();model_set_flash(id);model_set_sram_window(&p,sizeof p);}
int main(void) {
 const unsigned key=0x53470000,allowed[]={0x3ff000,0x1000,0};
 for(unsigned i=0;i<3;++i) {
  reset(0x944016);memset(model_nor(),0xa5,0x400000);memcpy(before,model_nor(),sizeof before);
  unsigned address=allowed[i];
  assert(!call(3,address,0,key));memset(before+address,0xff,4096);
  assert(!call(2,address+0xf00,256,key));memset(before+address+0xf00,0x42,256);
  assert(!memcmp(before,model_nor(),sizeof before));
  assert(model_command_count(0x20)==1&&model_command_count(2)==1);
 }
 const unsigned refused[]={0x2000,0x100000,0x1ff000,0x3fe000,0x400000};
 reset(0x944016);
 for(unsigned i=0;i<sizeof refused/sizeof refused[0];++i)assert(call(3,refused[i],0,key));
 assert(call(3,0,0,0)&&call(3,0,0,key|1)&&call(3,0,0,0xffffffff));
 assert(call(3,1,0,key)&&call(3,0,1,key));
 assert(call(2,0xff0,32,key)&&call(2,0,0,key));
 assert(!model_command_count(6)&&!model_command_count(0x20)&&!model_command_count(2));
 reset(0x5e4015);assert(call(3,0,0,key)&&!model_command_count(6));
 reset(0xef4016);assert(call(2,0,1,key)&&!model_command_count(6));
 reset(0x944016);model_set_flash_status(0x0c);
 assert(!call(9,0,1,0)&&p.data[0]==0x0c);
 assert(call(3,0,0,key)&&!model_command_count(6));
 reset(0x944016);uint8_t pm[0x60];memcpy(pm,model_pm(),sizeof pm);
 assert(call(7,0,14,0)&&!memcmp(pm,model_pm(),sizeof pm));
 puts("Silvergear recovery model: exact chip/key/sector limits, page bounds, protection refusal, PM writes blocked, other sectors preserved");
}
