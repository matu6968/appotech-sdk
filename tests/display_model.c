/* ILI9225G command/GRAM model: 8-bit bus, high byte first, R03 traversal.
 * Tests the actual driver against panel behavior, without a vendor routine. */
#define _GNU_SOURCE
#include <appotech/display.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static uint32_t sprs[65536],ticks,pio_writes;
static uint16_t regs[256],gram[220][176],word,command;
static unsigned half,last_rs,stuck,dma_stuck,dma_starts,cleans,dma_output;
int apx_dma_clean(const void *buffer,size_t length) {
    assert(buffer && length);assert(!(sprs[0xc800]&2));++cleans;return 0;
}
uint32_t apx_ticks(void) { return ++ticks; }
void apx_maintenance(void) {}
uint32_t apx_spr_read(uint32_t spr) {
    assert(spr<65536);
    return spr==0xa804?(sprs[spr]|(stuck?0:0x80000000u)):sprs[spr];
}
void apx_spr_write(uint32_t spr,uint32_t value) {
    assert(spr<65536);
    uint32_t previous=sprs[spr];
    if(spr==0xa804) {
        sprs[spr]=(previous&0x9fu)&~((value>>8)&0x9fu);
        sprs[spr]|=value&0x00ff0000u;return;
    }
    sprs[spr]=value;
    if(spr==0xa800 && (value&2)) {
        assert(sprs[0xc800]==3 && sprs[0xc818]==0x52081);
        assert(sprs[0xc920]==65536 && sprs[0xc924]==65536);
        assert(sprs[0xc810]==sprs[0xc820] && sprs[0xc928]==sprs[0xc92c]);
        assert(sprs[0xa85c]==0x02100008 && sprs[0xa86c]==0 && sprs[0xa868]==0);
        unsigned width=(sprs[0xc810]&65535)+1,height=(sprs[0xc810]>>16)+1;
        assert(sprs[0xa840]==width && sprs[0xa844]==height);
        assert(sprs[0xa854]+1==width*height*2 && cleans==2);
        ++dma_starts;cleans=0;
        if(dma_stuck) { sprs[0xc80c]=512;return; }
        const uint8_t *luma=(const uint8_t *)(uintptr_t)sprs[0xc828];
        const uint8_t *vu=(const uint8_t *)(uintptr_t)sprs[0xc830];
        unsigned pitch=sprs[0xc82c]*4;assert(pitch==sprs[0xc834]*4 && pitch>=width && !(pitch&31));
        dma_output=1;
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
            double yy=luma[y*pitch+x],v=vu[(y/2)*pitch+(x&~1u)]-128.0,
                   u=vu[(y/2)*pitch+(x&~1u)+1]-128.0;
            int r=yy+1.402*v,g=yy-0.344136*u-0.714136*v,b=yy+1.772*u;
            r=r<0?0:r>255?255:r;g=g<0?0:g>255?255:g;b=b<0?0:b>255?255:b;
            unsigned pixel=APX_RGB565(r,g,b);
            apx_spr_write(0xa858,pixel>>8);apx_spr_write(0xa858,pixel&255);
        }
        dma_output=0;
        sprs[0xc800]=1;sprs[0xc80c]=1;sprs[0xa804]|=8;sprs[0xa800]&=~2u;
        return;
    }
    if(spr==0x8004 && (previous&64) && !(value&64)) {
        memset(regs,0,sizeof(regs));command=word=half=0;
    }
    if(spr!=0xa858)return;
    if(!dma_output)++pio_writes;
    unsigned rs=(sprs[0x8010]>>2)&1;
    if(rs!=last_rs) { half=0;last_rs=rs; }
    if(!half) { word=(value&255u)<<8;half=1;return; }
    word|=value&255u;half=0;
    if(!rs) { command=word;assert(command<256);return; }
    if(command!=0x22) { regs[command]=word;return; }
    unsigned x=regs[0x20],y=regs[0x21];
    assert(x<176 && y<220);
    gram[y][x]=word;
    /* R03 AM selects the primary axis; ID0/ID1 select +/- direction.
     * Window wrapping follows datasheet R36/R37 and R38/R39. */
    int dx=(regs[3]&0x10)?1:-1,dy=(regs[3]&0x20)?1:-1;
    if(regs[3]&8) {
        int next=(int)y+dy;
        if(next<(int)regs[0x39] || next>(int)regs[0x38]) {
            y=dy>0?regs[0x39]:regs[0x38];
            next=(int)x+dx;
            x=(next<(int)regs[0x37] || next>(int)regs[0x36])?
                (dx>0?regs[0x37]:regs[0x36]):(unsigned)next;
        } else y=next;
    } else {
        int next=(int)x+dx;
        if(next<(int)regs[0x37] || next>(int)regs[0x36]) {
            x=dx>0?regs[0x37]:regs[0x36];
            next=(int)y+dy;
            y=(next<(int)regs[0x39] || next>(int)regs[0x38])?
                (dy>0?regs[0x39]:regs[0x38]):(unsigned)next;
        } else x=next;
    }
    regs[0x20]=x;regs[0x21]=y;
}
static void finish(void) {
    for(unsigned i=0;i<100000 && (apx_display_status()==APX_DISPLAY_INITIALIZING ||
                                apx_display_status()==APX_DISPLAY_BUSY);++i) {
        uint32_t before=pio_writes;apx_display_poll();
        assert(pio_writes-before<=32); /* USB service between bounded CPU bursts */
    }
    assert(apx_display_status()==APX_DISPLAY_READY);
}
static void mapped(unsigned rotation,unsigned x,unsigned y,unsigned *gx,unsigned *gy) {
    switch(rotation) {
    case 0:*gx=x;*gy=y;break;
    case 1:*gx=y;*gy=219-x;break;
    case 2:*gx=175-x;*gy=219-y;break;
    default:*gx=175-y;*gy=x;break;
    }
}
int main(void) {
    uint16_t *ddr=mmap((void *)0x02230000,0x20000,PROT_READ|PROT_WRITE,
                      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(ddr==(void *)0x02230000);
    sprs[0x8348]=0x55aa0003;sprs[0x8000]=0xaaaa;sprs[0x8004]=0x5555;
    sprs[0x4800]=(1u<<4)|(1u<<21)|(1u<<23)|(1u<<7);
    apx_display_init();assert(apx_display_fill(0,0,1,1,1)<0);finish();
    assert(sprs[0x4800]==((1u<<23)|(1u<<7)));
    assert(sprs[0x8348]==((0x55aa0003&~0x3c0u)|(13u<<6)));
    assert((sprs[0x8000]&~64u)==(0xaaaa&~64u));
    assert(sprs[0xa800]==0x11 && sprs[0xa85c]==0 && sprs[0xa86c]==0);
    assert(regs[1]==0x011c && regs[0x14]==0x5f45 && regs[7]==0x1017);
    const uint16_t pixels[]={0xf800,0x07e0,0xdead,0x001f,0xffff,0xbeef,0x1234,0xabcd};
    for(unsigned rotation=0;rotation<4;++rotation) {
        assert(apx_display_set_rotation(rotation)==0);
        assert(apx_display_width()==(rotation&1?220:176));
        assert(apx_display_height()==(rotation&1?176:220));
        uint32_t before=apx_display_pixels_sent();memset(gram,0,sizeof(gram));
        assert(apx_display_blit(7,11,2,3,pixels,3)==0);
        assert(apx_display_set_rotation(0)<0);
        assert(apx_display_fill(0,0,1,1,0)<0);finish();
        assert(apx_display_pixels_sent()-before==6);
        for(unsigned y=0;y<3;++y)for(unsigned x=0;x<2;++x) {
            unsigned gx,gy;mapped(rotation,7+x,11+y,&gx,&gy);
            assert(gram[gy][gx]==pixels[y*3+x]);
        }
        unsigned w=apx_display_width(),h=apx_display_height();
        assert(apx_display_fill(w-1,h-1,1,1,0x7654)==0);finish();
        unsigned gx,gy;mapped(rotation,w-1,h-1,&gx,&gy);assert(gram[gy][gx]==0x7654);
        assert(apx_display_fill(w,0,1,1,0)<0);
        assert(apx_display_fill(0,h,1,1,0)<0);
        assert(apx_display_fill(w-1,h-1,2,1,0)<0);
        assert(apx_display_fill(UINT32_MAX,0,1,1,0)<0);
        assert(apx_display_fill(0,0,0,1,0)<0);
        assert(apx_display_blit(0,0,2,1,pixels,1)<0);
        assert(apx_display_blit(0,0,1,1,NULL,1)<0);
        assert(apx_display_blit(0,0,1,1,(const uint16_t *)((uintptr_t)pixels+1),1)<0);
        assert(apx_display_blit(0,0,1,2,(const uint16_t *)(UINTPTR_MAX-3),3)<0);
    }
    assert(apx_display_set_rotation(4)<0);
    /* Pair-aligned colors preserve the expected 4:2:0 chroma. */
    for(unsigned y=0;y<6;++y)for(unsigned x=0;x<4;++x)ddr[y*4+x]=(y<2?0xf800:y<4?0x07e0:0x001f);
    uint16_t original[24];memcpy(original,ddr,sizeof(original));
    for(unsigned rotation=0;rotation<4;++rotation) {
        assert(apx_display_set_rotation(rotation)==0);
        unsigned start=dma_starts;uint32_t before=apx_display_pixels_sent();
        assert(apx_display_blit_dma(7,11,4,6,ddr)==0);finish();
        assert(dma_starts-start==1 && apx_display_pixels_sent()-before==24);
        assert(!memcmp(original,ddr,sizeof(original)) && sprs[0xc800]==0 && sprs[0xc828]==0);
        for(unsigned y=0;y<6;++y)for(unsigned x=0;x<4;++x) {
            unsigned gx,gy;mapped(rotation,7+x,11+y,&gx,&gy);
            uint16_t got=gram[gy][gx],expected=ddr[y*4+x];
            int dr=(int)(got>>11)-(expected>>11),dg=(int)((got>>5)&63)-((expected>>5)&63),db=(int)(got&31)-(expected&31);
            assert(dr>=-1 && dr<=1 && dg>=-1 && dg<=1 && db>=-1 && db<=1);
        }
    }
    memset(ddr,255,77440);
    for(unsigned rotation=0;rotation<4;++rotation) {
        assert(apx_display_set_rotation(rotation)==0);
        unsigned width=apx_display_width(),height=apx_display_height(),start=dma_starts;
        assert(apx_display_blit_dma(0,0,width,height,ddr)==0);finish();
        assert(dma_starts-start==1 && sprs[0xc82c]*4==((width+31)&~31u));
        assert(gram[0][0]==0xffff && gram[219][175]==0xffff);
    }
    assert(apx_display_blit_dma(0,0,3,2,ddr)<0);
    assert(apx_display_blit_dma(0,0,4,3,ddr)<0);
    assert(apx_display_blit_dma(0,0,4,2,(const uint16_t *)0x023ffffc)<0);
    assert(apx_display_blit_dma(0,0,4,2,(const uint16_t *)0x02210000)<0);
    assert(apx_display_blit_dma(0,0,UINT32_MAX,UINT32_MAX,ddr)<0);
    uint8_t *yp=(uint8_t *)ddr,*vu=yp+1024;
    memset(yp,255,128);memset(vu,128,64);
    assert(apx_display_blit_nv21(0,0,4,4,yp,vu,32)==0);finish();
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
        unsigned gx,gy;mapped(3,x,y,&gx,&gy);assert(gram[gy][gx]==0xffff);
    }
    assert(apx_display_blit_nv21(0,0,4,4,yp+1,vu,32)<0);
    assert(apx_display_blit_nv21(0,0,4,4,yp,vu,4)<0);
    assert(apx_display_blit_nv21(0,0,4,4,yp,vu,2)<0);
    assert(apx_display_blit_nv21(0,0,4,4,yp,vu,6)<0);
    assert(apx_display_blit_nv21(0,0,4,4,yp,(const uint8_t *)0x02400000,32)<0);
    assert(apx_display_blit_nv21(0,0,4,4,yp,vu,UINT32_MAX-3)<0);
    dma_stuck=1;assert(apx_display_blit_nv21(0,0,4,4,yp,vu,32)==0);
    for(unsigned i=0;i<50000 && apx_display_status()==APX_DISPLAY_BUSY;++i)apx_display_poll();
    assert(apx_display_status()==APX_DISPLAY_ERROR && apx_display_timeouts()==1);
    assert(sprs[0xc800]==0 && sprs[0xc828]==0 && sprs[0xc830]==0);
    dma_stuck=0;apx_display_init();finish();
    assert(apx_display_fill(0,0,1,1,0)==0);stuck=1;
    apx_display_poll();assert(apx_display_status()==APX_DISPLAY_ERROR);
    assert(apx_display_timeouts()==1 && sprs[0xa800]==0 && !(sprs[0x8000]&64));
    stuck=0;apx_display_init();finish();assert(apx_display_timeouts()==0);
    assert(regs[1]==0x011c && regs[0x14]==0x5f45 && regs[7]==0x1017);
    apx_display_shutdown();assert(apx_display_status()==APX_DISPLAY_OFF);
    munmap(ddr,0x20000);
    puts("display model passed: PIO/NV21/RGB DMA, full frames/rotations, 32-byte pitch, source preservation, bounds, timeout shutdown/reinit");
}
