/* Real FatFs over an in-memory block device. Never accesses a physical card. */
#include "ff.h"
#include "diskio.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <appotech/storage.h>
#include <appotech/fatfs_port.h>
#include <appotech/sd.h>
#include <appotech/debug.h>
static BYTE *image;
static LBA_t sectors;
static unsigned protected,writes;
static struct apx_sd_info card;
static uint32_t ticks;
static void put32(BYTE *p,uint32_t value) { for(unsigned i=0;i<4;++i)p[i]=value>>(8*i); }
uint32_t apx_ticks(void) { return ticks++; }
void apx_usb_poll(void) {}
void apx_maintenance(void) {}
void ff_appotech_progress(void) { apx_usb_poll();apx_maintenance(); }
const struct apx_sd_info *apx_sd_get_info(void) { card.sectors=sectors;card.write_protected=protected;return &card; }
int apx_sd_init(void) { card.initialized=1;return 0; }
int apx_sd_sync(void) { return card.initialized?0:APX_SD_NOT_READY; }
DSTATUS disk_status(BYTE drive) { return drive?STA_NOINIT:(protected?STA_PROTECT:0); }
DSTATUS disk_initialize(BYTE drive) { if(!drive)apx_sd_init();return disk_status(drive); }
DRESULT disk_read(BYTE drive,BYTE *buffer,LBA_t sector,UINT count) {
    if(drive||!count||sector>=sectors||count>sectors-sector)return RES_PARERR;
    memcpy(buffer,image+(size_t)sector*512,(size_t)count*512);return RES_OK;
}
DRESULT disk_write(BYTE drive,const BYTE *buffer,LBA_t sector,UINT count) {
    if(protected)return RES_WRPRT;
    if(drive||!count||sector>=sectors||count>sectors-sector)return RES_PARERR;
    memcpy(image+(size_t)sector*512,buffer,(size_t)count*512);++writes;return RES_OK;
}
DRESULT disk_ioctl(BYTE drive,BYTE cmd,void *buffer) {
    if(drive)return RES_PARERR;
    switch(cmd) {
    case CTRL_SYNC:return RES_OK;
    case GET_SECTOR_COUNT:*(LBA_t *)buffer=sectors;return RES_OK;
    case GET_BLOCK_SIZE:*(DWORD *)buffer=1;return RES_OK;
    case GET_SECTOR_SIZE:*(WORD *)buffer=512;return RES_OK;
    default:return RES_PARERR;
    }
}
void *ff_memalloc(UINT size) { return malloc(size); }
void ff_memfree(void *p) { free(p); }
int apx_sd_read(uint32_t sector,void *buffer,unsigned count) { return disk_read(0,buffer,sector,count)==RES_OK?0:APX_SD_RANGE; }
int apx_sd_write(uint32_t sector,const void *buffer,unsigned count) { return disk_write(0,buffer,sector,count)==RES_OK?0:APX_SD_CARD_ERROR; }
static void exercise(unsigned format,LBA_t count,unsigned kind) {
    sectors=count;image=calloc(sectors,512);assert(image);protected=0;
    BYTE work[4096];MKFS_PARM params={.fmt=format|FM_SFD,.n_fat=1,.align=1,.n_root=512,.au_size=512};
    assert(apx_storage_format(format|FM_SFD,512)==FR_OK);
    FATFS *fs=apx_storage_volume();assert(fs&&fs->fs_type==kind);
    assert(apx_storage_mount()==FR_OK);assert(!ff_appotech_volume_busy(fs));
    DIR root;assert(f_opendir(&root,"0:/")==FR_OK);assert(ff_appotech_volume_busy(fs));
    unsigned old_writes=writes;assert(apx_storage_format(format,0)==FR_LOCKED&&writes==old_writes);
    assert(apx_storage_unmount()==FR_LOCKED&&apx_storage_prepare_bootrom()==0x100+FR_LOCKED);
    assert(f_closedir(&root)==FR_OK&&!ff_appotech_volume_busy(fs));
    assert(f_setlabel("0:SDK TEST")==FR_OK);
    assert(f_mkdir("0:/Long directory 日本語")==FR_OK);
    assert(f_chdir("0:/Long directory 日本語")==FR_OK);
    const char *name="Long filename résumé 日本語 emoji 😀.txt";
    FIL file;assert(f_open(&file,name,FA_CREATE_NEW|FA_WRITE)==FR_OK);
    FIL duplicate;assert(f_open(&duplicate,name,FA_WRITE)==FR_LOCKED);
    BYTE content[8193];for(unsigned i=0;i<sizeof(content);++i)content[i]=(i*73u+i/256u)&255;
    UINT written;assert(f_write(&file,content,sizeof(content),&written)==FR_OK&&written==sizeof(content));
    assert(apx_storage_format(format,0)==FR_LOCKED);
    assert(f_close(&file)==FR_OK);
    FILE *update=apx_fopen("0:/stdio.bin","w+b");assert(update);
    assert(fwrite(content,1,4097,update)==4097&&fflush(update)==0);
    assert(fseek(update,511,SEEK_SET)==0);assert(fwrite("XYZ",1,3,update)==3&&fflush(update)==0);
    assert(fseek(update,511,SEEK_SET)==0);BYTE three[3];assert(fread(three,1,3,update)==3&&!memcmp(three,"XYZ",3));
    assert(apx_storage_format(format,0)==FR_LOCKED);assert(fclose(update)==0);
    assert(!apx_fopen("0:/stdio.bin","wx")&&errno==EEXIST);
    update=apx_fopen("0:/stdio.bin","a+");assert(update);
    assert(fseek(update,0,SEEK_SET)==0&&fwrite("tail",1,4,update)==4&&fflush(update)==0);
    assert(fseek(update,-4,SEEK_END)==0&&fread(three,1,3,update)==3&&!memcmp(three,"tai",3));
    assert(fclose(update)==0);
    /* Valid path with a huge offset must fail without allocating/truncating. */
    BYTE request[64]={0},reply[4096];size_t reply_size;
    const char *stdio_path="0:/stdio.bin";size_t path_bytes=strlen(stdio_path)+1;
    memset(request,255,8);put32(request+12,path_bytes);memcpy(request+16,stdio_path,path_bytes);
    unsigned writes_before=writes;
    assert(apx_storage_command(APX_STORAGE_FILE_WRITE,request,16+path_bytes,reply,&reply_size)==APXD_RANGE);
    assert(!reply_size&&writes==writes_before);
    /* A nonzero offset with replace mode is rejected before FA_CREATE_ALWAYS. */
    memset(request,0,12);put32(request,1);put32(request+8,2);
    assert(apx_storage_command(APX_STORAGE_FILE_WRITE,request,16+path_bytes,reply,&reply_size)==APXD_INVALID);
    assert(!reply_size&&writes==writes_before);
    FILINFO preserved;assert(f_stat(stdio_path,&preserved)==FR_OK&&preserved.fsize==4101);
    for(unsigned i=0;i<5;++i)assert(!apx_fopen("0:/bad",(const char *[]){"","rbb","rx","w++","q"}[i])&&errno==EINVAL);
    assert(apx_storage_sync()==FR_OK&&apx_storage_unmount()==FR_OK);
    protected=1;unsigned before=writes;
    assert(apx_storage_mount()==FR_OK);
    char label[768];DWORD serial;assert(f_getlabel("0:",label,&serial)==FR_OK&&!strcmp(label,"SDK TEST"));
    DIR dir;FILINFO entry;assert(f_opendir(&dir,"0:/Long directory 日本語")==FR_OK);
    assert(f_readdir(&dir,&entry)==FR_OK&&!strcmp(entry.fname,name)&&entry.fsize==sizeof(content));
    assert(f_closedir(&dir)==FR_OK);
    assert(f_open(&file,"0:/Long directory 日本語/Long filename résumé 日本語 emoji 😀.txt",FA_READ)==FR_OK);
    BYTE actual[8200];UINT got;
    assert(f_read(&file,actual,sizeof(actual),&got)==FR_OK&&got==sizeof(content));assert(!memcmp(actual,content,got));
    assert(f_lseek(&file,511)==FR_OK);assert(f_read(&file,actual,513,&got)==FR_OK&&got==513&&!memcmp(actual,content+511,513));
    assert(f_close(&file)==FR_OK);
    FILE *stream=apx_fopen("0:/Long directory 日本語/Long filename résumé 日本語 emoji 😀.txt","rb");assert(stream);
    assert(fread(actual,1,513,stream)==513&&!memcmp(actual,content,513));
    assert(fseek(stream,-1,SEEK_END)==0&&fgetc(stream)==content[sizeof(content)-1]);
    assert(fgetc(stream)==EOF&&feof(stream));assert(fclose(stream)==0);
    assert(!apx_fopen("0:/new","w")&&errno==EROFS);
    assert(f_open(&file,"0:/new",FA_CREATE_ALWAYS|FA_WRITE)==FR_WRITE_PROTECTED);
    assert(apx_storage_format(format,0)==FR_WRITE_PROTECTED);
    assert(f_mkfs("0:",&params,work,sizeof(work))==FR_WRITE_PROTECTED);
    assert(f_setlabel("0:CHANGED")==FR_WRITE_PROTECTED);assert(writes==before);
    assert(apx_storage_unmount()==FR_OK);protected=0;
    assert(apx_storage_mount()==FR_OK);
    assert(f_rename("0:/stdio.bin","0:/renamed.bin")==FR_OK);
    assert(f_unlink("0:/renamed.bin")==FR_OK);
    assert(apx_storage_format(format|FM_SFD,3)==FR_INVALID_PARAMETER);
    assert(apx_storage_format(0,0)==FR_INVALID_PARAMETER);
    assert(apx_storage_format(format|FM_SFD,512)==FR_OK);
    assert(apx_storage_unmount()==FR_OK);free(image);
    printf("FatFs filesystem %u: SDK format/mount/sync, UTF-8 LFN, root/file locks, read/write/append stdio, rename/unlink and write protection passed\n",kind);
}
int main(void) {
    for(unsigned i=0;i<5;++i)assert(f_setcp((WORD[]){437,932,936,949,950}[i])==FR_OK);
    assert(f_setcp(123)==FR_INVALID_PARAMETER);assert(f_setcp(437)==FR_OK);
    exercise(FM_FAT,2048,FS_FAT12);exercise(FM_FAT,16384,FS_FAT16);
    exercise(FM_FAT32,262144,FS_FAT32);exercise(FM_EXFAT,131072,FS_EXFAT);
}
