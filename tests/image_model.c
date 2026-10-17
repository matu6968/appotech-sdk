#include <appotech/image.h>
#include <appotech/storage.h>
#include <appotech/sd.h>
#include <appotech/debug.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define start(...) apx_image_decode_start_ex(__VA_ARGS__,APX_IMAGE_ALLOW_SLOW)
static unsigned ticks,opened,closed,writes,reads,fail_write,corrupt_read;
static struct apx_sd_info card={.initialized=1,.clock_hz=24000000};
static struct {FIL *handle;FILE *file;char name[64];} files[3];
uint32_t apx_ticks(void){return ticks++;}
void apx_usb_poll(void){}
void apx_buttons_poll(void){}
void apx_maintenance(void){}
const struct apx_sd_info *apx_sd_get_info(void){return &card;}
int apx_sd_set_clock(unsigned hz){assert(hz==12000000||hz==24000000);card.clock_hz=hz;return 0;}
FRESULT apx_storage_mount(void){card.initialized=1;return FR_OK;}
FRESULT apx_storage_unmount(void){for(unsigned i=0;i<3;++i)if(files[i].handle){fclose(files[i].file);files[i].handle=NULL;++closed;}return FR_OK;}
static FILE *host(FIL *f){for(unsigned i=0;i<3;++i)if(files[i].handle==f)return files[i].file;assert(0);return NULL;}
FRESULT f_open(FIL *f,const TCHAR *name,BYTE mode){assert(mode==(FA_READ|FA_WRITE|FA_CREATE_NEW));
 for(unsigned i=0;i<3;++i)if(!files[i].handle){files[i].handle=f;files[i].file=tmpfile();strcpy(files[i].name,name);assert(files[i].file);f->fptr=f->obj.objsize=0;++opened;return FR_OK;}return FR_TOO_MANY_OPEN_FILES;
}
FRESULT f_lseek(FIL *f,FSIZE_t position){if(position==CREATE_LINKMAP)return FR_NOT_ENOUGH_CORE;FILE *p=host(f);
 assert(!fseek(p,position,SEEK_SET));f->fptr=position;if(position>f->obj.objsize){assert(position);assert(!fseek(p,position-1,SEEK_SET));assert(fputc(0,p)!=EOF);f->obj.objsize=position;assert(!fseek(p,position,SEEK_SET));}return FR_OK;}
FRESULT f_read(FIL *f,void *buffer,UINT count,UINT *got){*got=fread(buffer,1,count,host(f));f->fptr+=*got;++reads;
 if(corrupt_read&&*got>1){((uint8_t *)buffer)[0]^=0x55;if(corrupt_read!=UINT32_MAX)--corrupt_read;}return FR_OK;}
FRESULT f_write(FIL *f,const void *buffer,UINT count,UINT *got){if(fail_write){card.initialized=0;*got=0;return FR_DISK_ERR;}*got=fwrite(buffer,1,count,host(f));f->fptr+=*got;if(f->fptr>f->obj.objsize)f->obj.objsize=f->fptr;++writes;return FR_OK;}
FRESULT f_sync(FIL *f){return fflush(host(f))?FR_DISK_ERR:FR_OK;}
FRESULT f_close(FIL *f){for(unsigned i=0;i<3;++i)if(files[i].handle==f){assert(!fclose(files[i].file));files[i].handle=NULL;++closed;return FR_OK;}assert(0);return FR_INT_ERR;}
FRESULT f_unlink(const TCHAR *name){assert(!strncmp(name,"0:/APXJ",7));return FR_OK;}
int main(int argc,char **argv){assert(argc>=4);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);
 uint8_t *input=malloc(length),*output=aligned_alloc(32,131072),*workspace=aligned_alloc(32,atoi(argv[3]));assert(input&&output&&workspace);assert(fread(input,1,length,f)==(size_t)length);fclose(f);
 struct apx_image_info info;assert(!apx_image_inspect(input,length,&info));unsigned sw=info.image.width,sh=info.image.height,w,h;
 if(sw*176>sh*220){w=220;h=(220*sh/sw)&~1u;}else{h=176;w=(176*sw/sh)&~1u;}if(w>sw||h>sh){w=sw&~1u;h=sh&~1u;}
 unsigned initial_opened=opened;int guarded=apx_image_decode_start(input,length,output,131072,w,h,workspace,atoi(argv[3]));
 if(info.slow_reasons){assert(guarded==APX_JPEG_SLOW&&opened==initial_opened);}else{assert(!guarded);apx_image_shutdown();}
 assert(apx_image_decode_start_ex(input,length,output,131072,w,h,workspace,atoi(argv[3]),2)==APX_JPEG_INVALID);
 assert(!start(input,length,output,131072,w,h,workspace,atoi(argv[3])));
 unsigned loops=0;do{apx_image_poll();apx_image_get_info(&info);assert(++loops<200000);}while(info.image.state==APX_JPEG_DECODING);
 if(info.image.error){fprintf(stderr,"Decode failed %u %s loops%u workspace%u files%u\n",info.image.error,argv[1],loops,info.workspace_used,info.backing_files);return 1;}
 assert(info.image.state==APX_JPEG_DONE&&opened==closed&&card.clock_hz==24000000);f=fopen(argv[2],"wb");assert(f);fwrite(output,1,info.image.bytes,f);fclose(f);
 printf("%ux%u -> %ux%u pitch%u: scans%u backing%u read%u/write%u workspace%u loops%u\n",sw,sh,w,h,info.image.pitch,info.scans,info.backing_files,reads,writes,info.workspace_used,loops);
 // Cancellation must close/unlink its backing stores and permit a new job.
 assert(!start(input,length,output,131072,w,h,workspace,atoi(argv[3])));apx_image_poll();apx_image_shutdown();assert(opened==closed);
 assert(start(input,length,output,8,w,h,workspace,atoi(argv[3]))==APX_JPEG_RANGE);
 assert(apx_image_prepare_stop()==0);
 if(info.backing_files){
  corrupt_read=1;assert(!start(input,length,output,131072,w,h,workspace,atoi(argv[3])));loops=0;
  do{apx_image_poll();apx_image_get_info(&info);assert(++loops<200000);}while(info.image.state==APX_JPEG_DECODING);
  assert(info.image.state==APX_JPEG_DONE&&info.backing_retries==1&&opened==closed);
  corrupt_read=UINT32_MAX;assert(!start(input,length,output,131072,w,h,workspace,atoi(argv[3])));loops=0;
  do{apx_image_poll();apx_image_get_info(&info);assert(++loops<200000);}while(info.image.state==APX_JPEG_DECODING);
  assert(info.image.state==APX_JPEG_ERROR&&info.image.error==APX_JPEG_HARDWARE&&info.backing_retries==2&&opened==closed);corrupt_read=0;
  fail_write=1;assert(!start(input,length,output,131072,w,h,workspace,atoi(argv[3])));
  loops=0;do{apx_image_poll();apx_image_get_info(&info);assert(++loops<200000);}while(info.image.state==APX_JPEG_DECODING);
  assert(info.image.state==APX_JPEG_ERROR&&info.image.error==APX_JPEG_HARDWARE&&opened==closed);fail_write=0;}
 assert(!start(input,length-2,output,131072,w,h,workspace,atoi(argv[3])));loops=0;
 do{apx_image_poll();apx_image_get_info(&info);assert(++loops<200000);}while(info.image.state==APX_JPEG_DECODING);
 /* BMP/PNM loaders may ignore trailing padding; only JPEG requires its EOI. */
 if(input[0]==255&&input[1]==216)assert(info.image.state==APX_JPEG_ERROR&&opened==closed);
 apx_image_shutdown();assert(opened==closed);
 assert(!start(input,length,output,131072,w,h,workspace,atoi(argv[3])));apx_image_poll();ticks+=32768u*601u;apx_image_poll();apx_image_get_info(&info);
 assert(info.image.state==APX_JPEG_ERROR&&info.image.error==APX_JPEG_TIMEOUT&&opened==closed);
 free(input);free(output);free(workspace);return 0;
}
