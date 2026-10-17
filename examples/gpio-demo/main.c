#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/display.h>
#include <appotech/buttons.h>
#include <appotech/storage.h>
#include <appotech/power.h>
#include <stdio.h>
#include <string.h>
static uint16_t label_pixels[60*16];
static void label(unsigned key,uint16_t color) {
    static const char *names[]={"SHUTTER","DOWN","UP","LEFT","RIGHT","POWER"};
    static const char letters[]="SHTERDOWNUPLFIG";
    static const uint8_t glyphs[][5]={
        {0x46,0x49,0x49,0x49,0x31},{0x7f,8,8,8,0x7f},{1,1,0x7f,1,1},
        {0x7f,0x49,0x49,0x49,0x41},{0x7f,9,0x19,0x29,0x46},
        {0x7f,0x41,0x41,0x22,0x1c},{0x3e,0x41,0x41,0x41,0x3e},
        {0x3f,0x40,0x38,0x40,0x3f},{0x7f,2,4,8,0x7f},
        {0x3f,0x40,0x40,0x40,0x3f},{0x7f,9,9,9,6},{0x7f,0x40,0x40,0x40,0x40},
        {0x7f,9,9,9,1},{0,0x41,0x7f,0x41,0},{0x3e,0x41,0x49,0x49,0x7a}
    };
    for(unsigned i=0;i<60*16;++i)label_pixels[i]=color;
    const char *name=names[key];unsigned x=(60-strlen(name)*6)/2;
    for(;*name;++name,x+=6) {
        const char *found=strchr(letters,*name);
        if(!found)continue;
        for(unsigned y=0;y<14;++y)for(unsigned c=0;c<5;++c)
            if(glyphs[found-letters][c]&(1u<<(y/2)))label_pixels[(y+1)*60+x+c]=0xffff;
    }
}
int main(void) {
    apx_usb_init();apx_buttons_init();apx_display_init();
    printf("GPIO: six buttons, PB5 LED; 20 ms debounce; SD read/write/format\n");
    unsigned painted=0xffffffffu,drawing=0,index=0,background=0,mounted=0,label_pending=0;
    uint32_t power_since=0;unsigned power_held=0,power_armed=0; /* armed by the first release after boot */
    for(;;) {
        apx_debug_poll();apx_buttons_poll();apx_display_poll();
        if(!mounted&&apx_storage_volume()) { printf("SD filesystem mounted\n");mounted=1; }
        struct apx_buttons_info buttons;apx_buttons_get_info(&buttons);
        if(!(buttons.state&APX_BUTTON_POWER))power_armed=1;
        if(power_armed&&(buttons.state&APX_BUTTON_POWER)) {
            if(!power_held) { power_since=apx_ticks();power_held=1; }
            else if((uint32_t)(apx_ticks()-power_since)>=65536) {
                uint32_t error=apx_power_off();
                printf("Power off failed: %lu\n",(unsigned long)error);power_since=apx_ticks();
            }
        } else power_held=0;
        if(apx_display_status()!=APX_DISPLAY_READY)continue;
        if(!background) {
            apx_display_fill(0,0,220,176,APX_RGB565(20,20,20));background=1;continue;
        }
        if(!index&&painted!=buttons.state) { drawing=buttons.state;index=1; }
        if(index) {
            unsigned key=index-1;
            uint16_t color=drawing&(1u<<key)?APX_RGB565(0,220,60):APX_RGB565(40,60,100);
            if(!label_pending) {
                apx_display_fill(10+(key%3)*70,10+(key/3)*82,60,72,color);label_pending=1;
            } else {
                label(key,color);apx_display_blit(10+(key%3)*70,38+(key/3)*82,60,16,label_pixels,60);
                label_pending=0;
                if(++index==7) { painted=drawing;index=0; }
            }
        }
    }
}
