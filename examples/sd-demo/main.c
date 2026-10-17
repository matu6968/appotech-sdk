#include <appotech/soc.h>
#include <appotech/usb.h>
#include <appotech/debug.h>
#include <appotech/display.h>
#include <appotech/storage.h>
#include <stdio.h>
#include <string.h>
#include <appotech/fatfs_port.h>
/* Optional destructive stdio acceptance mailbox. Request is written last
 * using APXD's explicit raw RAM override; default startup only reads files. */
struct stdio_control { uint32_t magic,request,completed,failures; };
volatile struct stdio_control apx_sd_stdio_control;
static uint32_t stdio_write_test(void) {
    uint32_t failures=0;
    static uint8_t content[1025],actual[1029];
    for(unsigned i=0;i<sizeof(content);++i)content[i]=(i*37u+0x95u)&255;
    FILE *file=apx_fopen("0:/APXSTDIO.TXT","w+b");if(!file)return 1;
    if(fwrite(content,1,sizeof(content),file)!=sizeof(content)||fflush(file))failures|=2;
    if(apx_storage_format(FM_FAT,0)!=FR_LOCKED||apx_storage_prepare_bootrom()!=0x100u+FR_LOCKED)failures|=4;
    if(fseek(file,511,SEEK_SET)||fwrite("XYZ",1,3,file)!=3||fflush(file))failures|=8;
    memcpy(content+511,"XYZ",3);
    if(fseek(file,0,SEEK_SET)||fread(actual,1,sizeof(content),file)!=sizeof(content)||memcmp(content,actual,sizeof(content)))failures|=16;
    if(fclose(file))failures|=32;
    file=apx_fopen("0:/APXSTDIO.TXT","a+");if(!file)return failures|64;
    if(fseek(file,0,SEEK_SET)||fwrite("tail",1,4,file)!=4||fflush(file))failures|=128;
    if(fclose(file))failures|=128;
    file=apx_fopen("0:/APXSTDIO.TXT","rb");if(!file)return failures|256;
    if(fread(actual,1,sizeof(actual),file)!=sizeof(actual)||memcmp(actual,content,sizeof(content))||
        memcmp(actual+sizeof(content),"tail",4))failures|=512;
    if(fclose(file))failures|=1024;
    DIR root;
    if(f_opendir(&root,"0:/")!=FR_OK)failures|=2048;
    else {
        if(apx_storage_format(FM_FAT,0)!=FR_LOCKED)failures|=4096;
        if(f_closedir(&root)!=FR_OK)failures|=8192;
    }
    if(apx_storage_sync()!=FR_OK)failures|=16384;
    return failures;
}
static void stdio_demo(void) {
    DIR directory;FILINFO entry;
    if(f_opendir(&directory,"0:/")!=FR_OK)return;
    unsigned found=0;
    while(f_readdir(&directory,&entry)==FR_OK&&entry.fname[0])
        if(!(entry.fattrib&AM_DIR)) { found=1;break; }
    f_closedir(&directory);
    if(found) {
        FILE *file=apx_fopen(entry.fname,"rb");
        if(!file) { printf("SD stdio open failed\n");return; }
        unsigned char bytes[16];size_t got=fread(bytes,1,sizeof(bytes),file);
        int seek=entry.fsize?fseek(file,-1,SEEK_END):0;
        int closed=fclose(file);
        printf("SD stdio: %s, first read %u bytes, seek %d, close %d\n",entry.fname,(unsigned)got,seek,closed);
    }
}
int main(void) {
    apx_usb_init();
    apx_sd_stdio_control.magic=0x4f494453; /* SDIO */
    printf("SD read/write/format ready; FatFs R0.16 UTF-8/all codepages\n");
    apx_display_init();unsigned displayed=0,stdio_done=0;
    for(;;) {
        apx_debug_poll();apx_display_poll();
        uint32_t sequence=apx_sd_stdio_control.request;
        if(sequence!=apx_sd_stdio_control.completed&&apx_storage_volume()) {
            apx_sd_stdio_control.failures=stdio_write_test();
            apx_sd_stdio_control.completed=sequence;
            printf("picolibc SD stdio test: failures=%lu\n",(unsigned long)apx_sd_stdio_control.failures);
        }
        if(!stdio_done&&apx_storage_volume()) { stdio_demo();stdio_done=1; }
        unsigned next=apx_storage_volume()?2:1;
        if(displayed!=next&&apx_display_status()==APX_DISPLAY_READY)
            if(apx_display_fill(0,0,apx_display_width(),apx_display_height(),
                next==2?APX_RGB565(0,80,160):APX_RGB565(40,40,40))==0)displayed=next;
    }
}
