#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/display.h>
#include <appotech/buttons.h>
#include <appotech/storage.h>
#include <appotech/power.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "../printer-camera-demo/font.h" /* digits and A-Z, five columns by seven rows */

/* One tile per key. Action: six keys in a 3x2 grid of 60x72 tiles on the 220x176 panel (unchanged from the original demo).
 * Silvergear: seven ladder keys in a 4x2 grid of 70x100 tiles on the 320x240 panel; a short Power click also toggles the
 * white flash LED (PB0), the same gesture as the stock camera. */
#if APX_BOARD_SILVERGEAR
#define KEYS 7u
#define COLS 4u
#define TILE_W 70u
#define TILE_H 100u
#define STEP_X 78u
#define STEP_Y 110u
static const char *const names[KEYS]={"POWER","LEFT","RIGHT","MODE","SHUTTER","EXIT","PRINT"};
static const unsigned flags[KEYS]={APX_BUTTON_POWER,APX_BUTTON_LEFT,APX_BUTTON_RIGHT,APX_BUTTON_MODE,APX_BUTTON_SHUTTER,APX_BUTTON_EXIT,APX_BUTTON_PRINT};
#define BANNER "GPIO: seven ladder keys, PB0 flash LED (Power click); 20 ms debounce; SD read/write/format\n"
#else
#define KEYS 6u
#define COLS 3u
#define TILE_W 60u
#define TILE_H 72u
#define STEP_X 70u
#define STEP_Y 82u
static const char *const names[KEYS]={"SHUTTER","DOWN","UP","LEFT","RIGHT","POWER"};
static const unsigned flags[KEYS]={APX_BUTTON_SHUTTER,APX_BUTTON_DOWN,APX_BUTTON_UP,APX_BUTTON_LEFT,APX_BUTTON_RIGHT,APX_BUTTON_POWER};
#define BANNER "GPIO: six buttons, PB5 LED; 20 ms debounce; SD read/write/format\n"
#endif
static uint16_t label_pixels[TILE_W*16];
static void label(unsigned key,uint16_t color) {
    for(unsigned i=0;i<TILE_W*16;++i)label_pixels[i]=color;
    const char *name=names[key];unsigned x=(TILE_W-strlen(name)*6)/2;
    for(;*name;++name,x+=6) {
        const uint8_t *g=font[glyph(*name)];
        for(unsigned y=0;y<14;++y)for(unsigned c=0;c<5;++c)
            if(g[c]&(1u<<(y/2)))label_pixels[(y+1)*TILE_W+x+c]=0xffff;
    }
}
int main(void) {
    apx_usb_init();apx_buttons_init();apx_display_init();
    printf(BANNER);
    unsigned painted=0xffffffffu,drawing=0,index=0,background=0,mounted=0,label_pending=0;
    uint32_t power_since=0;unsigned power_held=0,power_armed=0; /* armed by the first release after boot */
#if APX_BOARD_SILVERGEAR
    uint32_t click_since=0;unsigned click_down=0;
#endif
    for(;;) {
        apx_debug_poll();apx_buttons_poll();apx_display_poll();
        if(!mounted&&apx_storage_volume()) { printf("SD filesystem mounted\n");mounted=1; }
        struct apx_buttons_info buttons;apx_buttons_get_info(&buttons);
        if(!(buttons.state&APX_BUTTON_POWER))power_armed=1;
        if(power_armed&&(buttons.state&APX_BUTTON_POWER)) {
            if(!power_held) { power_since=apx_ticks();power_held=1; }
            else if((uint32_t)(apx_ticks()-power_since)>=65536) {
                uint32_t error=apx_power_off(); /* waits for the key to be released on Silvergear, as stock does */
                printf("Power off failed: %lu\n",(unsigned long)error);power_since=apx_ticks();
            }
        } else power_held=0;
#if APX_BOARD_SILVERGEAR
        /* Short Power click (pressed after the first release, released within 1 s) toggles the flash LED. */
        if(power_armed&&(buttons.state&APX_BUTTON_POWER)&&!click_down) { click_down=1;click_since=apx_ticks(); }
        else if(!(buttons.state&APX_BUTTON_POWER)&&click_down) {
            if((uint32_t)(apx_ticks()-click_since)<32768)(void)apx_led_set(apx_led_get()?0:1);
            click_down=0;
        }
#endif
        if(apx_display_status()!=APX_DISPLAY_READY)continue;
        if(!background) {
            apx_display_fill(0,0,apx_display_width(),apx_display_height(),APX_RGB565(20,20,20));background=1;continue;
        }
        if(!index&&painted!=buttons.state) { drawing=buttons.state;index=1; }
        if(index) {
            unsigned key=index-1;
            uint16_t color=drawing&flags[key]?APX_RGB565(0,220,60):APX_RGB565(40,60,100);
            unsigned x=10+(key%COLS)*STEP_X,y=10+(key/COLS)*STEP_Y;
            if(!label_pending) {
                apx_display_fill(x,y,TILE_W,TILE_H,color);label_pending=1;
            } else {
                label(key,color);apx_display_blit(x,y+TILE_H/2-8,TILE_W,16,label_pixels,TILE_W);
                label_pending=0;
                if(++index==KEYS+1) { painted=drawing;index=0; }
            }
        }
    }
}
