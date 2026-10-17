#include <appotech/trace.h>
#include <appotech/soc.h>
#include <assert.h>
#include <stdio.h>
uint32_t marks_ram[64];
static uint32_t timer;
uint32_t apx_spr_read(uint32_t spr){return spr==0x9804?timer++:0;}
int main(void){
    /* Nothing is recorded before the SPI boot starts a list (a USB launch must not scribble in SRAM). */
    apx_mark(1,2);assert(marks_ram[0]==0&&marks_ram[1]==0);
    apx_mark_reset();assert(marks_ram[0]==0x4b52414du&&marks_ram[1]==0);
    timer=0x3ffe;apx_mark(0x81,0x1234567);
    assert(marks_ram[1]==1&&marks_ram[2]==(0x81u<<24|0x234567u)&&marks_ram[3]==0x3ffe);
    apx_mark(0x82,5);assert(marks_ram[1]==2&&marks_ram[4]==(0x82u<<24|5)&&marks_ram[5]==0x3fff);
    for(unsigned i=0;i<100;++i)apx_mark(9,i);
    assert(marks_ram[1]==31&&marks_ram[2*30+2]!=0&&marks_ram[63]!=0); /* the table stops at 31 entries, inside 256 bytes */
    apx_mark_reset();assert(marks_ram[1]==0);
    puts("boot marks model: guarded by reset, 24-bit args, timer stamp, bounded to 31 entries");
    return 0;
}
