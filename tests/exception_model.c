/* Host model of src/exception.c (the assembly entry stub is not compiled here). */
#include <appotech/exception.h>
#include <appotech/soc.h>
#include <appotech/watchdog.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
uint32_t exc_vectors[64],exc_record[32],exc_previous[32],exc_flag[2];
static char uart[2048];static unsigned uart_n,reset_called;
static jmp_buf escape;
uint32_t apx_spr_read(uint32_t spr){return spr==0x908c?2:0;}
void apx_spr_write(uint32_t spr,uint32_t value){if(spr==0x9088&&uart_n<sizeof(uart)-1)uart[uart_n++]=(char)value;}
_Noreturn void apx_watchdog_reset(void){++reset_called;longjmp(escape,1);}
extern _Noreturn void apx_exception_report(void);
int main(void){
    for(unsigned i=0;i<64;++i)exc_vectors[i]=0x11110000u+i;
    uint32_t original[64];memcpy(original,exc_vectors,sizeof(exc_vectors));
    apx_exception_install();
    /* slot 0x20 = word 8: l.ori r3,r0,0x20 ; l.movhi r4,0x0218 ; l.ori r4,r4,0x0abc ; l.jr r4 */
    assert(exc_vectors[8]==0xa8600020u&&exc_vectors[9]==0x18800218u&&exc_vectors[10]==0xa8840abcu&&exc_vectors[11]==0x44002000u);
    assert(exc_vectors[0x70/4]==0xa8600070u&&exc_vectors[0xe0/4]==0xa86000e0u);
    /* vendor-owned slots stay: reset 0x00/0x10, tick 0x50, external interrupt 0x80, reserved 0xf0 and everything between slots */
    for(unsigned w=0;w<64;++w){
        unsigned slot=(w*4)&~15u;int ours=0;
        for(unsigned s=0;s<11;++s){static const unsigned list[]={0x20,0x30,0x40,0x60,0x70,0x90,0xa0,0xb0,0xc0,0xd0,0xe0};ours|=slot==list[s];}
        assert(ours?exc_vectors[w]!=original[w]:exc_vectors[w]==original[w]);
    }
    uint32_t after[64];memcpy(after,exc_vectors,sizeof(exc_vectors));
    apx_exception_install();assert(!memcmp(after,exc_vectors,sizeof(exc_vectors))); /* idempotent: the saved copy is not overwritten */
    apx_exception_restore();assert(!memcmp(original,exc_vectors,sizeof(exc_vectors)));
    apx_exception_restore();assert(!memcmp(original,exc_vectors,sizeof(exc_vectors)));
    /* A deliberate reset is flagged for the handler and the flag does not survive a boot. */
    apx_exception_expect_reset();assert(exc_flag[0]==0x52535421u);
    apx_exception_boot();assert(exc_flag[0]==0);
    /* A clean boot leaves nothing to report. */
    struct apx_exception e;
    memset(exc_record,0,sizeof(exc_record));apx_exception_boot();assert(!apx_exception_previous(&e));
    /* A crashed run: the handler reports, the next boot moves the exc_record aside. */
    exc_record[0]=0x48535243u;exc_record[1]=1;exc_record[2]=0x70;exc_record[3]=0x0218abcd;exc_record[4]=0x123;exc_record[5]=0x8001;exc_record[6]=0x100;exc_record[7]=0x0218ffee;exc_record[8]=7;
    uart_n=0;
    if(!setjmp(escape))apx_exception_report();
    assert(reset_called==1&&exc_record[0]==0x48535243u);
    uart[uart_n]=0;
    assert(strstr(uart,"EXC 00000070 illegal instruction S20=0218abcd S30=00000123 S40=00008001 SP=00000100 LR=0218ffee"));
    apx_exception_boot();
    assert(apx_exception_previous(&e)&&e.vector==0x70&&e.spr20==0x0218abcd&&e.spr30==0x123&&e.spr40==0x8001&&e.sp==0x100&&e.lr==0x0218ffee&&e.fp==7);
    assert(exc_record[0]==0&&!exc_record[1]);                  /* ready for the next crash */
    apx_exception_boot();assert(!apx_exception_previous(&e)); /* only the boot right after the crash reports it */
    assert(!strcmp(apx_exception_name(0x20),"bus error")&&!strcmp(apx_exception_name(0x60),"alignment")&&!strcmp(apx_exception_name(0x44),"unknown"));
    puts("exception model: slots patched and restored, exc_record kept for exactly one boot, one-line report then reset");
    return 0;
}
