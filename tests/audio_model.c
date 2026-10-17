#include <appotech/audio.h>
#include <appotech/audio_files.h>
#include <appotech/microphone.h>
#include <appotech/microphone_files.h>
#include <appotech/storage.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include "board.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
static uint32_t regs[65536],codec[16],ticks,writes,cleaned;
static unsigned busy_control,open_files,closed_files,short_read;
static unsigned busy_record,invalidated,capture_exists,short_write,sync_error;
static uint8_t capture_wave[65600];static size_t capture_bytes;
static uint8_t wav[65580];static size_t wav_bytes;
uint32_t apx_ticks(void) {return ticks++;}
void apx_delay_ms(uint32_t n) {ticks+=n*33;}
void apx_maintenance(void) {apx_audio_poll();apx_mic_poll();}
void apx_usb_poll(void) {apx_maintenance();}
void apx_buttons_poll(void) {}
void apx_display_poll(void) {}
int apx_dma_clean(const void *p,size_t n) {assert(!((uintptr_t)p&31)&&!(n&31));++cleaned;return 0;}
int apx_dma_invalidate(void *p,size_t n) {assert(!((uintptr_t)p&31)&&!(n&31));++invalidated;return 0;}
uint32_t apx_spr_read(uint32_t r) {return regs[r];}
void apx_spr_write(uint32_t r,uint32_t v) {
 ++writes;
 if(r==0x9114){
  uint32_t flags=regs[r]&15;flags&=~((v>>8)&15);
  if(v&0x80000000)flags=busy_record?0:4;
  regs[r]=(v&0x30000)|flags;return;}
 if(r==0x915c){unsigned index=(v>>24)&15;
  if(busy_control){regs[r]=0x80000000;return;}
  if(v&0x40000000)codec[index]=v&0xffffff;
  regs[0x9160]=codec[index];regs[r]=v&0x7f000000;return;}
 if(r==0x914c){regs[0x9150]&=~(v&7);if(v&8){assert(cleaned&&!(regs[0x4800]&(1u<<6)));assert(regs[0x9148]>=APX_RAM_BASE);assert(codec[0]&1);}regs[r]=0;return;}
 regs[r]=v;
}
FRESULT apx_storage_mount(void) {return FR_OK;}
FRESULT f_open(FIL *f,const TCHAR *name,BYTE mode) {
 if(mode==(FA_WRITE|FA_CREATE_NEW)) {
  assert(!strcmp(name,"capture.wav"));if(capture_exists)return FR_EXIST;
  capture_exists=1;capture_bytes=0;memset(f,0,sizeof(*f));++open_files;return FR_OK;
 }
 assert(mode==FA_READ);if(strcmp(name,"test.wav"))return FR_NO_FILE;
 memset(f,0,sizeof(*f));f->obj.objsize=wav_bytes;++open_files;return FR_OK;
}
FRESULT f_read(FIL *f,void *p,UINT bytes,UINT *got) {
 size_t left=wav_bytes-f->fptr;if(bytes>left)bytes=left;if(short_read&&bytes)--bytes;
 memcpy(p,wav+f->fptr,bytes);f->fptr+=bytes;*got=bytes;return FR_OK;
}
FRESULT f_lseek(FIL *f,FSIZE_t offset) {if(offset>wav_bytes)return FR_INVALID_PARAMETER;f->fptr=offset;return FR_OK;}
FRESULT f_close(FIL *f) {(void)f;assert(open_files);--open_files;++closed_files;return FR_OK;}
FRESULT f_write(FIL *f,const void *p,UINT bytes,UINT *done) {
 assert(f->fptr+bytes<=sizeof(capture_wave));if(short_write&&bytes)--bytes;
 memcpy(capture_wave+f->fptr,p,bytes);f->fptr+=bytes;capture_bytes=f->fptr;*done=bytes;return FR_OK;
}
FRESULT f_sync(FIL *f) {(void)f;return sync_error?FR_DISK_ERR:FR_OK;}
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i)p[i]=n>>(8*i);}
static void wave(void) {
 memset(wav,0,sizeof(wav));memcpy(wav,"RIFF",4);put32(wav+4,100);memcpy(wav+8,"WAVEfmt ",8);put32(wav+16,16);
 wav[20]=wav[22]=1;put32(wav+24,16000);put32(wav+28,32000);wav[32]=2;wav[34]=16;
 memcpy(wav+36,"data",4);put32(wav+40,64);for(unsigned i=44;i<108;++i)wav[i]=i;wav_bytes=108;
}
static void complete(void) {
 struct apx_audio_info i;regs[0x9150]=4;regs[0x9158]=7;apx_audio_poll();apx_audio_get_info(&i);assert(i.state==APX_AUDIO_DRAINING);
 ticks+=100;apx_audio_poll();apx_audio_get_info(&i);assert(i.state==APX_AUDIO_DONE&&!i.error);
 assert(regs[0x9148]==0&&regs[0x9158]==0&&(regs[0x8874]&255)==0);
}
int main(void) {
 assert(mmap((void *)APX_RAM_BASE,0x400000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void *)APX_RAM_BASE);
 regs[0x4800]=~0u;regs[0x8840]=0x81000000;regs[0x8848]=0x13570300;regs[0x884c]=0xa5000001;
 uint32_t old_select=regs[0x8848],old_div=regs[0x884c];
 int16_t *data=(void *)0x02300000;struct apx_audio_info i;
 unsigned before=writes;assert(apx_audio_play(data,64,44100)==APX_AUDIO_UNSUPPORTED&&writes==before);
 assert(apx_audio_play(data+1,64,16000)==APX_AUDIO_RANGE);assert(apx_audio_play(data,33,16000)==APX_AUDIO_RANGE);
 assert(apx_audio_play((void *)0x02400000,64,16000)==APX_AUDIO_RANGE);
 assert(apx_audio_play(data,SIZE_MAX,16000)==APX_AUDIO_RANGE);
 assert(apx_audio_init()==0);assert((regs[0x8848]&~0x300u)==(old_select&~0x300u));assert((regs[0x884c]&~0xf000000u)==(old_div&~0xf000000u));
 assert(apx_audio_volume(101)==APX_AUDIO_INVALID);assert(apx_audio_volume(60)==0);assert((regs[0x8874]&255)==0);
 assert(apx_audio_play(data,64,16000)==0&&codec[0]==(APX_AUDIO_SELECTOR_16000<<4|1u)&&codec[3]==60*32767u/100u);
 before=writes;assert(apx_audio_play(data,64,16000)==APX_AUDIO_BUSY&&writes==before);
 assert(apx_audio_mute(2)==APX_AUDIO_INVALID);assert(apx_audio_mute(1)==0&&codec[3]==0);assert(apx_audio_mute(0)==0);complete();
 assert(apx_audio_play(data,64,8000)==0&&codec[0]==(APX_AUDIO_SELECTOR_8000<<4|1u));complete();
 assert(apx_audio_play(data,64,32000)==0&&codec[0]==(APX_AUDIO_SELECTOR_32000<<4|1u));regs[0x9150]=0;ticks+=65536;apx_audio_poll();apx_audio_get_info(&i);assert(i.state==APX_AUDIO_ERROR&&i.error==APX_AUDIO_TIMEOUT);
 apx_audio_stop();assert(apx_audio_play(data,64,16000)==0);apx_audio_shutdown();assert(!(codec[0]&1)&&regs[0x8848]==old_select&&regs[0x884c]==old_div);
 busy_control=1;assert(apx_audio_init()==APX_AUDIO_TIMEOUT);busy_control=0;apx_audio_shutdown();assert(apx_audio_files_init()==0);
 wave();assert(apx_audio_wav_file("test.wav")==0);assert(!open_files);complete();
 wave();wav[22]=2;before=writes;assert(apx_audio_wav_file("test.wav")==0x300+APX_AUDIO_UNSUPPORTED&&writes==before&&!open_files);
 wave();put32(wav+40,4096);assert(apx_audio_wav_file("test.wav")==0x300+APX_AUDIO_INVALID&&!open_files);
 wave();short_read=1;assert(apx_audio_wav_file("test.wav")==0x100+FR_DISK_ERR&&!open_files);short_read=0;
 assert(apx_audio_wav_file("missing.wav")==0x100+FR_NO_FILE);
 wave();uint8_t response[4096],input[16]={0};size_t size=0;
 assert(apx_audio_command(6,input,8,response,&size)==0);put32(input,2);assert(apx_audio_command(6,input,8,response,&size)==0x300+APX_AUDIO_RANGE);
 put32(input,16000);assert(apx_audio_command(7,input,4,response,&size)==0);
 before=writes;assert(apx_audio_command(6,input,8,response,&size)==0x300+APX_AUDIO_BUSY&&writes==before);complete();
 assert(apx_audio_command(0,input,0,response,&size)==0&&size>100);assert(apx_audio_command(2,(void *)"bad\0x\0",6,response,&size)==APXD_INVALID);
 assert(apx_audio_tone(440,500,16000)==0);complete();assert(apx_audio_tone(440,4000,32000)==0x300+APX_AUDIO_RANGE);
 assert(!open_files&&closed_files>=4);
 struct apx_mic_info mic;
 before=writes;assert(apx_mic_capture_start(data,64,44100)==APX_MIC_UNSUPPORTED&&writes==before);
 assert(apx_mic_capture_start(data+1,64,16000)==APX_MIC_RANGE);
 assert(apx_mic_capture_start(data,SIZE_MAX,16000)==APX_MIC_RANGE);
 assert(apx_mic_capture_start((void *)0x023fffe0,64,16000)==APX_MIC_RANGE);
 assert(apx_mic_capture_start(data,64,16000)==0);assert(regs[0x910c]==15&&regs[0x9108]==(uintptr_t)data);
 before=writes;assert(apx_mic_capture_start(data,64,16000)==APX_MIC_BUSY&&writes==before);
 assert(apx_audio_play(data,64,16000)==APX_AUDIO_BUSY);
 assert(apx_audio_play((void *)0x02301000,64,16000)==0);
 unsigned invalidates=invalidated;regs[0x9114]=2;apx_mic_poll();apx_mic_get_info(&mic);
 assert(mic.state==APX_MIC_CAPTURING&&invalidated==invalidates);
 regs[0x9114]|=1;apx_mic_poll();apx_mic_get_info(&mic);assert(mic.state==APX_MIC_DONE&&invalidated>invalidates);
 assert(regs[0x9108]==0&&!(regs[0x9100]&15)&&mic.completed==1);complete();
 assert(apx_audio_play(data,64,16000)==0);before=writes;
 assert(apx_mic_capture_start(data+16,64,16000)==APX_MIC_BUSY&&writes==before);apx_audio_stop();
 assert(apx_mic_capture_start(data,64,8000)==0);assert((regs[0x9100]&~15u)==APX_MIC_CONTROL_8000);
 ticks+=65536;apx_mic_poll();apx_mic_get_info(&mic);assert(mic.state==APX_MIC_ERROR&&mic.error==APX_MIC_TIMEOUT&&!regs[0x9108]);apx_mic_stop();
 busy_record=1;assert(apx_mic_capture_start(data,64,32000)==APX_MIC_TIMEOUT);busy_record=0;apx_mic_stop();
 put32(input,64);put32(input+4,16000);assert(apx_microphone_command(1,input,8,response,&size)==0);
 assert(apx_microphone_command(3,input,8,response,&size)==0x400+APX_MIC_BUSY);
 apx_mic_get_info(&mic);memset((void *)(uintptr_t)mic.buffer,7,64);regs[0x9114]=1;apx_mic_poll();
 put32(input,0);put32(input+4,64);assert(apx_microphone_command(3,input,8,response,&size)==0&&size==64&&response[63]==7);
 assert(apx_microphone_command(4,(void *)"capture.wav",12,response,&size)==0);
 assert(capture_bytes==108&&!memcmp(capture_wave,"RIFF",4)&&capture_wave[24]==0x80&&capture_wave[25]==0x3e&&capture_wave[107]==7&&!open_files);
 assert(apx_microphone_command(4,(void *)"capture.wav",12,response,&size)==0x100+FR_EXIST);
 capture_exists=0;short_write=1;assert(apx_microphone_command(4,(void *)"capture.wav",12,response,&size)==0x100+FR_DISK_ERR&&!open_files);short_write=0;
 capture_exists=0;sync_error=1;assert(apx_microphone_command(4,(void *)"capture.wav",12,response,&size)==0x100+FR_DISK_ERR&&!open_files);sync_error=0;
 assert(apx_microphone_command(5,input,0,response,&size)==0);
 put32(input,64);put32(input+4,16000);assert(apx_microphone_command(1,input,8,response,&size)==0x400+APX_MIC_BUSY);complete();
 ((uint8_t *)(uintptr_t)mic.buffer)[64]=0;put32(input,0);put32(input+4,64);
 assert(apx_microphone_command(3,input,8,response,&size)==0x400+APX_MIC_HARDWARE);
 assert(apx_microphone_command(4,(void *)"x\0tail\0",7,response,&size)==0x400+APX_MIC_HARDWARE);
 apx_mic_stop();assert(apx_mic_capture_start(data,64,32000)==0);apx_audio_shutdown();apx_mic_get_info(&mic);assert(mic.state==APX_MIC_OFF);
 assert(!open_files);puts("Audio/microphone models: PCM enable/rates/bounds, half/full DMA, duplex ownership, cache invalidation, cancel/timeout/shutdown, WAV parsing/saving/short writes and guard checks passed");
}
