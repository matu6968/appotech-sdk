#include "../src/printer_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "printer_render_fixtures.h"
int main(void){
 struct apx_printer_render r;
 for(unsigned n=0;n<sizeof(fixtures)/sizeof(fixtures[0]);++n){
  const struct fixture *f=fixtures+n;
  assert(!apx_printer_render_init(&r,f->source,f->width*f->height,f->width,f->height,f->width,
                                f->width,f->height,0,APX_PRINTER_DOT,0));
  for(unsigned y=0;y<f->height;++y){
   assert(!apx_printer_render_row(&r,y));
   for(unsigned x=0;x<f->width;++x)if(r.row[x]!=f->expected[y*f->width+x]){
    fprintf(stderr,"stock Dot mismatch fixture%u (%ux%u) at%u,%u: SDK%u stock%u\n",
            n,f->width,f->height,x,y,r.row[x],f->expected[y*f->width+x]);return 1;
   }
  }
  assert(apx_printer_render_row(&r,f->height)==APX_PRINTER_INVALID);
 }
 const uint8_t photo[]={0,64,255,123,128,192,32,45};
 const uint8_t clockwise[]={128,0,192,64,32,255};
 assert(!apx_printer_render_init(&r,photo,sizeof(photo),3,2,4,2,3,1,APX_PRINTER_GRAY,0));
 for(unsigned y=0;y<3;++y){assert(!apx_printer_render_row(&r,y));assert(!memcmp(r.row,clockwise+y*2,2));}
 assert(!apx_printer_render_init(&r,photo,sizeof(photo),3,2,4,4,6,1,APX_PRINTER_GRAY,0));
 for(unsigned y=0;y<6;++y){assert(!apx_printer_render_row(&r,y));for(unsigned x=0;x<4;++x)assert(r.row[x]==clockwise[(y/2)*2+x/2]);}
 assert(!apx_printer_render_init(&r,photo,sizeof(photo),3,2,4,3,2,0,APX_PRINTER_GRAY,1));
 assert(apx_printer_render_row(&r,1)==APX_PRINTER_INVALID);
 assert(!apx_printer_render_row(&r,0));assert(r.row[0]==10&&r.row[1]==60&&r.row[2]==209);
 uint8_t ramp[256];for(unsigned v=0;v<256;++v)ramp[v]=v;
 assert(!apx_printer_render_init(&r,ramp,sizeof(ramp),256,1,256,256,1,0,APX_PRINTER_GRAY,1));
 assert(!apx_printer_render_row(&r,0));
 for(unsigned v=1;v<256;++v)assert(r.row[v]>=r.row[v-1]);
 assert(r.row[0]==10&&r.row[128]==110&&r.row[255]==209);
 assert(!apx_printer_render_init(&r,ramp,sizeof(ramp),256,1,256,256,1,0,APX_PRINTER_GRAY,2));
 assert(!apx_printer_render_row(&r,0));
 for(unsigned v=1;v<256;++v)assert(r.row[v]>=r.row[v-1]);
 assert(r.row[0]==10&&r.row[128]==60&&r.row[255]==209);
 assert(apx_printer_render_init(&r,photo,5,3,2,4,3,2,0,APX_PRINTER_GRAY,0)==APX_PRINTER_INVALID);
 assert(apx_printer_render_init(&r,photo,sizeof(photo),3,2,4,385,2,0,APX_PRINTER_GRAY,0)==APX_PRINTER_INVALID);
 assert(apx_printer_render_init(&r,photo,sizeof(photo),3,2,4,3,2,2,APX_PRINTER_GRAY,0)==APX_PRINTER_INVALID);
 for(unsigned p=0;p<32;++p)assert(apx_printer_gray_threshold(p)==expected_thresholds[p]);
 assert(!apx_printer_gray_threshold(32));
 puts("Printer render model: stock-native Dot golden outputs, serpentine/clipped edges, Gray thresholds/curve, rotation/scaling, pitch and capacity passed");
}
