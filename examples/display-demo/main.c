#include <appotech/display.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include <appotech/usb.h>
#include <stdint.h>
#include <stdio.h>

/* This example mailbox is optional; the library has no dependency on APXD.
 * Read status over APXD memory READ. WRITE request_sequence last to queue a
 * pattern. Tools/display_demo.py uses ELF symbols and verifies the build ID. */
struct demo_control {
    uint32_t magic,version,state,request_sequence,completed_sequence,pattern;
    uint32_t framebuffer,width,height,pixels_sent,timeouts,frames;
    uint32_t dma_enabled,dma_transfers,last_transfer_ticks;
};
volatile struct demo_control apx_display_demo_control;
/* Keep the reusable runtime/16 KiB heap in its original 128 KiB reservation.
 * The launch manifest owns and zero-preflights this extra DDR workspace. */
static uint16_t framebuffer[APX_DISPLAY_WIDTH*APX_DISPLAY_HEIGHT] __attribute__((section(".large_buffer"),aligned(32)));
static uint32_t submitted;
static int active;
static const uint16_t bars[]={0xf800,0x07e0,0x001f,0xffe0,0x07ff,0xf81f,0xffff,0x0000};
static void text(unsigned x,unsigned y,unsigned scale,const char *string) {
    static const char letters[]="JT529SDKLX1760RGB";
    static const uint8_t glyphs[][5]={
        {0x20,0x40,0x41,0x3f,0x01},{0x01,0x01,0x7f,0x01,0x01},
        {0x27,0x45,0x45,0x45,0x39},{0x42,0x61,0x51,0x49,0x46},
        {0x06,0x49,0x49,0x29,0x1e},{0x46,0x49,0x49,0x49,0x31},
        {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x08,0x14,0x22,0x41},
        {0x7f,0x40,0x40,0x40,0x40},{0x63,0x14,0x08,0x14,0x63},
        {0x00,0x42,0x7f,0x40,0x00},{0x01,0x71,0x09,0x05,0x03},
        {0x3c,0x4a,0x49,0x49,0x30},{0x3e,0x51,0x49,0x45,0x3e},
        {0x7f,0x09,0x19,0x29,0x46},{0x3e,0x41,0x49,0x49,0x7a},
        {0x7f,0x49,0x49,0x49,0x36}
    };
    for(;*string;++string,x+=6*scale) {
        const uint8_t *glyph=NULL;
        for(unsigned i=0;i<sizeof(letters)-1;++i)if(letters[i]==*string)glyph=glyphs[i];
        for(unsigned cy=0;cy<7*scale;++cy)for(unsigned cx=0;cx<6*scale;++cx) {
            if(x+cx<APX_DISPLAY_WIDTH && y+cy<APX_DISPLAY_HEIGHT)
                framebuffer[(y+cy)*APX_DISPLAY_WIDTH+x+cx]=
                    glyph && cx<5*scale && (glyph[cx/scale]&(1u<<(cy/scale)))?0xffff:0;
        }
    }
}
static void pattern(unsigned id) {
    for(unsigned y=0;y<APX_DISPLAY_HEIGHT;++y) {
        for(unsigned x=0;x<APX_DISPLAY_WIDTH;++x) {
            uint16_t color;
            switch(id) {
            case 1:color=APX_RGB565(x*255/(APX_DISPLAY_WIDTH-1),y*255/(APX_DISPLAY_HEIGHT-1),128);break;
            case 2:color=((x/8+y/8)&1)?0xffff:0;break;
            case 3:color=0xf800;break;
            case 4:color=0x07e0;break;
            case 5:color=0x001f;break;
            case 6:color=0xffff;break;
            case 7:color=0x0000;break;
            default:color=bars[x*8/APX_DISPLAY_WIDTH];
                if(x<12 && y<12)color=0xf800;
                if(x>=APX_DISPLAY_WIDTH-12 && y<12)color=0x07e0;
                if(x<12 && y>=APX_DISPLAY_HEIGHT-12)color=0x001f;
                if(x>=APX_DISPLAY_WIDTH-12 && y>=APX_DISPLAY_HEIGHT-12)color=0xffff;
                if((y==16 || y==APX_DISPLAY_HEIGHT-17) && x>=16 && x<APX_DISPLAY_WIDTH-16)color=0xffff;
                break;
            }
            framebuffer[y*APX_DISPLAY_WIDTH+x]=color;
        }
        /* Building a frame also cooperates with the USB debugger. */
        apx_debug_poll();
    }
    if(id==0) {
        text(44,72,2,"JT5295 SDK");
        text(77,91,1,"220X176 RGB");
        text(14,14,1,"TL");text(APX_DISPLAY_WIDTH-26,14,1,"TR");
        text(14,APX_DISPLAY_HEIGHT-21,1,"BL");text(APX_DISPLAY_WIDTH-26,APX_DISPLAY_HEIGHT-21,1,"BR");
    }
}
int main(void) {
    apx_usb_init();
    apx_display_demo_control.magic=0x31505344; /* DSP1 */
    apx_display_demo_control.version=2;
    apx_display_demo_control.framebuffer=(uintptr_t)framebuffer;
    apx_display_demo_control.width=APX_DISPLAY_WIDTH;
    apx_display_demo_control.height=APX_DISPLAY_HEIGHT;
    apx_display_demo_control.request_sequence=1;
    apx_display_demo_control.dma_enabled=1;
    apx_display_demo_control.pattern=0;
    printf("LCD %ux%u RGB565 ctl=%08lx fb=%08lx\n",
           APX_DISPLAY_WIDTH,APX_DISPLAY_HEIGHT,(unsigned long)(uintptr_t)&apx_display_demo_control,
           (unsigned long)(uintptr_t)framebuffer);
    apx_display_init();
    for(;;) {
        apx_debug_poll();apx_display_poll();
        enum apx_display_state state=apx_display_status();
        apx_display_demo_control.state=state;
        apx_display_demo_control.pixels_sent=apx_display_pixels_sent();
        apx_display_demo_control.timeouts=apx_display_timeouts();
        apx_display_demo_control.dma_transfers=apx_display_dma_transfers();
        apx_display_demo_control.last_transfer_ticks=apx_display_last_transfer_ticks();
        if(active && state==APX_DISPLAY_READY) {
            apx_display_demo_control.completed_sequence=submitted;
            ++apx_display_demo_control.frames;active=0;
            printf("frame %lu pixels=%lu timeouts=%lu\n",
                   (unsigned long)apx_display_demo_control.frames,
                   (unsigned long)apx_display_pixels_sent(),(unsigned long)apx_display_timeouts());
        }
        if(state==APX_DISPLAY_READY && apx_display_demo_control.request_sequence!=submitted) {
            submitted=apx_display_demo_control.request_sequence;
            unsigned id=apx_display_demo_control.pattern;
            if(id<8) {
                pattern(id);
                active=(apx_display_demo_control.dma_enabled?
                    apx_display_blit_dma(0,0,APX_DISPLAY_WIDTH,APX_DISPLAY_HEIGHT,framebuffer):
                    apx_display_blit(0,0,APX_DISPLAY_WIDTH,APX_DISPLAY_HEIGHT,framebuffer,APX_DISPLAY_WIDTH))==0;
            } else if(id==8) { /* exercise partial window and fill */
                active=apx_display_fill(32,40,64,80,0xf81f)==0;
            } else if(id==9) { /* host-prepared NV21 in the same owned buffer */
                active=apx_display_blit_nv21(0,0,APX_DISPLAY_WIDTH,APX_DISPLAY_HEIGHT,
                    (const uint8_t *)framebuffer,(const uint8_t *)framebuffer+224*176,224)==0;
            } else {
                apx_display_demo_control.completed_sequence=submitted;
                printf("display: rejected pattern %u\n",id);
            }
        }
    }
}
