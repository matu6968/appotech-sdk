/* Host model of apx_spi_overlay_load(): the flash image comes from the real packer (apxtool spi_boot.py),
 * argv[1] = flash image, argv[2] = expected overlay 0 (at 0x022e0000), argv[3] = expected overlay 1 (at 0x02300000). */
#include <appotech/spi.h>
#include <appotech/soc.h>
#include <appotech/debug.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t flash[APX_SPI_CAPACITY];
uint8_t bank[0x120000];
static unsigned spi_booted=1,init_calls,reads,polls,maintenance,flushed,icache_toggles,fail_read_at=0xffffffffu;
static uint32_t spr2000=1;
unsigned apx_boot_from_spi(void){return spi_booted;}
int apx_spi_init(void){++init_calls;return APX_SPI_OK;}
int apx_spi_read(uint32_t address,void *buffer,size_t length){
    ++reads;
    if(address==fail_read_at)return APX_SPI_TIMEOUT;
    if(address>APX_SPI_CAPACITY||length>APX_SPI_CAPACITY-address)return APX_SPI_INVALID;
    memcpy(buffer,flash+address,length);return APX_SPI_OK;
}
void apx_debug_poll(void){++polls;}
void apx_maintenance(void){++maintenance;}
void apx_cache_flush_range(const void *start,const void *end){(void)start;(void)end;++flushed;}
uint32_t apx_spr_read(uint32_t spr){return spr==0x2000?spr2000:0;}
void apx_spr_write(uint32_t spr,uint32_t value){if(spr==0x2000){if((spr2000^value)&1)++icache_toggles;spr2000=value;}}
static uint8_t *load(const char *path,size_t *size){
    FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*size=ftell(f);rewind(f);
    uint8_t *data=malloc(*size?*size:1);assert(fread(data,1,*size,f)==*size);fclose(f);return data;
}
int main(int argc,char **argv){
    assert(argc==4);
    size_t n,a_size,b_size;uint8_t *image=load(argv[1],&n),*a=load(argv[2],&a_size),*b=load(argv[3],&b_size);
    assert(n==APX_SPI_CAPACITY);
    memcpy(flash,image,n);
    /* Not an SPI boot: nothing is read at all. */
    spi_booted=0;assert(apx_spi_overlay_load()==APX_SPI_OK&&reads==0&&init_calls==0);
    spi_booted=1;
    /* Free space starts at the first 4 KiB boundary after the last overlay. */
    {
        uint32_t entries[8];memcpy(entries,flash+0x1fc000+16,32);
        uint32_t last=entries[4]+entries[6];
        assert(apx_spi_image_end()==((last+4095u)&~4095u)&&apx_spi_image_end()<APX_SPI_OVERLAY_TABLE);
        flash[0x1fc000+16+1]^=1;assert(apx_spi_image_end()>=0x2200u&&apx_spi_image_end()<((last+4095u)&~4095u)); /* bad table CRC: main image only */
        flash[0x1fc000+16+1]^=1;
    }
    assert(apx_spi_overlay_load()==APX_SPI_OK);
    assert(!memcmp(bank,a,a_size)&&!memcmp(bank+0x20000,b,b_size));
    assert(flushed==2&&icache_toggles==2&&(spr2000&1)&&polls>0&&maintenance>0);
    /* A corrupted payload byte is caught by the CRC, before anything is declared loaded. */
    uint32_t table[4];memcpy(table,flash+0x1fc000+16,16);
    flash[table[0]+7]^=0x10;memset(bank,0,sizeof(bank));flushed=0;
    assert(apx_spi_overlay_load()==APX_SPI_HARDWARE&&flushed==0);
    flash[table[0]+7]^=0x10;
    /* A corrupted table entry, a bad magic, and an entry that points outside the bank are all refused. */
    flash[0x1fc000+16+5]^=1;assert(apx_spi_overlay_load()==APX_SPI_INVALID);flash[0x1fc000+16+5]^=1;
    flash[0x1fc000]^=1;assert(apx_spi_overlay_load()==APX_SPI_INVALID);flash[0x1fc000]^=1;
    /* A read error is reported, not swallowed. */
    fail_read_at=table[0];assert(apx_spi_overlay_load()==APX_SPI_TIMEOUT);fail_read_at=0xffffffffu;
    /* An erased table means the image has no overlay. */
    memset(flash+0x1fc000,0xff,4096);reads=0;
    assert(apx_spi_overlay_load()==APX_SPI_OK&&reads==1);
    assert(apx_spi_overlay_load()==APX_SPI_OK);
    free(image);free(a);free(b);
    puts("SPI overlay model: CRC-checked copy, cache maintenance, USB launches untouched, bad tables refused");
    return 0;
}
