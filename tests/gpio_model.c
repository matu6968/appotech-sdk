#include <appotech/gpio.h>
#include <appotech/buttons.h>
#include <appotech/debug.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t regs[65536],now,conversion_done;
static unsigned adc=1023,stuck;
uint32_t apx_ticks(void) { return now; }
uint32_t apx_spr_read(uint32_t reg) {
    if(reg==0x9170&&(regs[reg]&0x40)&&!stuck&&(int32_t)(now-conversion_done)>=0)regs[reg]&=~0x40u;
    if(reg==0x9178)return adc;
    if(reg==0x9174)return 0;
    return regs[reg];
}
void apx_spr_write(uint32_t reg,uint32_t value) {
    regs[reg]=value;
    if(reg==0x9170&&(value&0x80)) { regs[reg]=(value&~0x80u)|0x40;conversion_done=now+4; }
}
static void run(unsigned ticks) {
    for(unsigned i=0;i<ticks;i+=4) { now+=4;apx_buttons_poll(); }
}
static struct apx_buttons_info state(void) {
    struct apx_buttons_info result;apx_buttons_get_info(&result);return result;
}
static void drain(void) { struct apx_button_event event;while(apx_buttons_next(&event)){} }
int main(void) {
    for(unsigned i=0;i<65536;++i)regs[i]=0xa5a555aau;
    uint32_t before[65536];memcpy(before,regs,sizeof(regs));
    assert(apx_gpio_configure(5,1,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,0)==-1);
    assert(apx_gpio_configure(0,0,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,0)==-1);
    assert(apx_gpio_configure(0,0x10000,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,0)==-1);
    assert(apx_gpio_configure(0,1,2,APX_GPIO_FLOATING,0)==-1);
    assert(apx_gpio_configure(0,1,APX_GPIO_OUTPUT,3,0)==-1);
    assert(apx_gpio_configure(0,1,APX_GPIO_OUTPUT,APX_GPIO_FLOATING,2)==-1);
    assert(apx_gpio_read(0,1,NULL)==-1);assert(apx_gpio_write(0,1,2)==-1);
    assert(apx_gpio_set_override(0,1,3)==-1);assert(!memcmp(before,regs,sizeof(regs)));
    for(unsigned group=0;group<5;++group) {
        assert(!apx_gpio_configure(group,0x204,APX_GPIO_OUTPUT,APX_GPIO_PULL_UP,0x200));
        assert((regs[0x8000+4*group]&0x204)==0x200);
        assert(!(regs[0x8040+4*group]&0x204));assert((regs[0x8080+4*group]&0x204)==0x204);
        assert(!(regs[0x80c0+4*group]&0x204));
        assert(!apx_gpio_set_override(group,4,APX_GPIO_FORCE_LOW));
        assert(!(regs[0x81c0+4*group]&4)&&(regs[0x8200+4*group]&4));
        assert(!apx_gpio_set_override(group,4,APX_GPIO_FORCE_HIGH));
        assert((regs[0x81c0+4*group]&4)&&!(regs[0x8200+4*group]&4));
        assert(!apx_gpio_configure(group,0x204,APX_GPIO_INPUT,APX_GPIO_PULL_DOWN,0));
        assert((regs[0x8040+4*group]&0x204)==0x204);
        assert(!(regs[0x8080+4*group]&0x204)&&(regs[0x80c0+4*group]&0x204)==0x204);
        uint32_t pins;assert(!apx_gpio_read(group,0x204,&pins));assert(pins==0x200);
    }
    for(unsigned i=0;i<65536;++i) {
        unsigned changed=0;
        const unsigned banks[]={0x8000,0x8040,0x8080,0x80c0,0x8100,0x8140,0x81c0,0x8200,0x8380};
        for(unsigned b=0;b<9;++b)for(unsigned g=0;g<5;++g)if(i==banks[b]+4*g)changed=1;
        assert((regs[i]&~(changed?0x204u:0u))==(before[i]&~(changed?0x204u:0u)));
    }
    memset(regs,0,sizeof(regs));regs[0x8840]=0x8000;regs[0x9170]=0x13;
    regs[0x8044]=0x3f;regs[0x8384]=0x63;regs[0x81c4]=0x20;
    memcpy(before,regs,sizeof(regs));now=0xfffff000u;
    assert(apx_led_set(1)==-1);apx_buttons_init();apx_buttons_init();run(1000);
    assert(!state().state&&state().adc==1023&&!state().adc_timeouts);
    assert(!apx_led_set(1)&&apx_led_get()==1&&regs[0x81c4]==0x20&&!regs[0x8204]);
    assert(apx_led_set(2)==-1);assert(!apx_led_set(0)&&!apx_led_get());
    const unsigned centers[]={0,306,484,658,817};
    for(unsigned i=0;i<5;++i) {
        drain();adc=centers[i];run(400);assert(!state().state);run(600);assert(state().state==(1u<<i));
        struct apx_button_event event;assert(apx_buttons_next(&event));
        assert(event.pressed==(1u<<i)&&!event.released&&!apx_buttons_next(&event));
        adc=1023;run(1000);assert(!state().state);assert(apx_buttons_next(&event)&&event.released==(1u<<i));
    }
    adc=484;regs[0x8004]|=1;run(1000);assert(state().state==(APX_BUTTON_UP|APX_BUTTON_POWER));
    regs[0x8004]&=~1u;adc=1023;run(1000);drain();
    for(unsigned i=0;i<24;++i) { adc=i%2?1023:306;run(1000); }
    assert(state().events_dropped==8);unsigned events=0;struct apx_button_event event;
    while(apx_buttons_next(&event))++events;
    assert(events==16);
    adc=0;run(1000);assert(state().state==APX_BUTTON_SHUTTER);
    stuck=1;run(2000);assert(state().adc_timeouts&&state().adc==0xffff&&!state().state);
    stuck=0;adc=1023;run(1000);assert(state().adc==1023);
    uint8_t output[4096],input[4]={1};size_t size;
    assert(apx_gpio_command(1,input,4,output,&size)==APXD_OK&&!size);
    input[0]=2;assert(apx_gpio_command(1,input,4,output,&size)==APXD_RANGE);
    assert(apx_gpio_command(0,input,1,output,&size)==APXD_INVALID);
    assert(apx_gpio_command(999,input,0,output,&size)==APXD_INVALID);
    assert(apx_gpio_command(0,input,0,output,&size)==APXD_OK&&size<4096);
    apx_buttons_shutdown();apx_buttons_shutdown();
    for(unsigned i=0;i<65536;++i)if(i!=0x9174)assert(regs[i]==before[i]);
    assert(apx_gpio_command(0,input,0,output,&size)==APXD_UNSUPPORTED);
    puts("GPIO model passed: bank preservation, overrides, six buttons, debounce/wrap, queue overflow, ADC timeout, shutdown");
}
