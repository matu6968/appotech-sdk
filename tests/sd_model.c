#define _GNU_SOURCE
#include <appotech/sd.h>
#include <appotech/soc.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ff.h>
#include <diskio.h>
static uint32_t registers[65536],ticks,polls;
static unsigned high_capacity,present=1,crc_error,stuck,data_stuck,dma_armed;
static unsigned write_crc_error,busy_stuck,status_stuck,r1_write_protected,cleaned,pending_write;
static unsigned read_crc_once,write_crc_once,replace_on_error,cid_generation;
static uint32_t write_sector,busy_until;
static struct { uint32_t sector;unsigned valid;uint8_t data[512]; } blocks[16];
static uint8_t csd[16],sram[16];
static uint32_t rca=0x12340000;
static void set_bits(unsigned high,unsigned low,uint32_t value) {
    for(unsigned b=low;b<=high;++b) {
        unsigned mask=1u<<(b%8),index=(127-b)/8;
        csd[index]=(csd[index]&~mask)|((value>>(b-low)&1u)?mask:0);
    }
}
uint32_t apx_ticks(void) { return ticks++; }
void apx_maintenance(void) {}
void apx_usb_poll(void) { ++polls; }
int apx_dma_clean(const void *buffer,size_t length) {
    assert(!((uintptr_t)buffer&15)&&length==512);cleaned=1;return 0;
}
uint8_t apx_mmio_read8(uintptr_t address) { assert(address>=0x2000&&address<0x2010);return sram[address-0x2000]; }
int apx_dma_invalidate(void *buffer,size_t length) {
    assert(!((uintptr_t)buffer&15)&&length==512);return 0;
}
uint32_t apx_spr_read(uint32_t address) {
    if(address==0x9884)return registers[address]|(registers[0x9880]&0x28000);
    if(address==0x9880&&(registers[address]&2)&&!busy_stuck&&(int32_t)(ticks-busy_until)>=0)
        registers[address]|=1u<<20;
    return registers[address];
}
static void execute(void) {
    unsigned cmd=registers[0x9898]&63;uint32_t argument=registers[0x989c];
    assert(cmd==0||cmd==2||cmd==3||cmd==7||cmd==8||cmd==9||cmd==13||cmd==16||cmd==17||cmd==24||cmd==41||cmd==55);
    registers[0x9880]&=~((1u<<17)|(1u<<19));
    if(stuck)return;
    registers[0x9880]|=1u<<17;
    if(!present) { registers[0x9880]|=1u<<19;registers[0x989c]=0;return; }
    uint32_t reply=0x900;
    switch(cmd) {
    case 0:reply=0;break;
    case 8:assert(argument==0x1aa);reply=argument;break;
    case 55:reply=0x120;break;
    case 41:reply=0x80ff8000|(high_capacity?0x40000000:0);break;
    case 3:reply=rca|0x520;break;
    case 2:case 9:
        for(unsigned i=0;i<16;++i)sram[i^3]=cmd==9?csd[i]:(uint8_t)(i+cid_generation);
        break;
    case 7:case 13:assert(argument==rca);if(cmd==13&&status_stuck)reply=0xe00;break;
    case 16:assert(!high_capacity&&argument==512);break;
    case 17: {
        assert(registers[0x9894]==512);
        assert(dma_armed);dma_armed=0;
        uint32_t sector=high_capacity?argument:argument/512;
        if(!high_capacity)assert(argument%512==0);
        if(!data_stuck) {
            uint8_t *out=(void *)(uintptr_t)registers[0x9890];
            for(unsigned i=0;i<512;++i)out[i]=(sector*97+i)&255;
            for(unsigned i=0;i<16;++i)if(blocks[i].valid&&blocks[i].sector==sector)memcpy(out,blocks[i].data,512);
            registers[0x9880]|=0x8000;
        }
        unsigned bad=crc_error||read_crc_once;
        if(read_crc_once)--read_crc_once;
        if(bad&&replace_on_error){++cid_generation;replace_on_error=0;}
        registers[0x9880]=(registers[0x9880]&~0x600000u)|(bad?0x200000:0x400000);
        break;
    }
    case 24:
        assert(!dma_armed&&cleaned);cleaned=0;
        write_sector=high_capacity?argument:argument/512;
        if(!high_capacity)assert(argument%512==0);
        if(r1_write_protected)reply|=1u<<26;
        else pending_write=1;
        break;
    }
    registers[0x989c]=reply;
}
void apx_spr_write(uint32_t address,uint32_t value) {
    if(address==0x9894&&!value)dma_armed=0;
    if(address==0x9884) {
        if((value&0x10)&&registers[0x9894])dma_armed=1;
        registers[0x9880]&=~(value&0x28000);
        registers[address]=value&~0x28000;
        if((value&0x30)==0x30&&pending_write) {
            assert(registers[0x9894]==512&&dma_armed);dma_armed=pending_write=0;
            registers[0x9880]&=~((1u<<20)|(1u<<16));busy_until=ticks+32;
            if(!data_stuck)registers[0x9880]|=1u<<15;
            if(write_crc_error||write_crc_once){registers[0x9880]|=1u<<16;if(write_crc_once)--write_crc_once;}
            else if(!data_stuck) {
                unsigned i;for(i=0;i<16;++i)if(!blocks[i].valid||blocks[i].sector==write_sector)break;
                assert(i<16);blocks[i].valid=1;blocks[i].sector=write_sector;
                memcpy(blocks[i].data,(void *)(uintptr_t)registers[0x9890],512);
            }
        }
        if(value==1||value==2||value==3||value==5)execute();
    } else registers[address]=value;
}
static void initialize(unsigned hc) {
    high_capacity=hc;present=1;crc_error=stuck=data_stuck=0;memset(csd,0,16);
    write_crc_error=busy_stuck=status_stuck=r1_write_protected=cleaned=pending_write=0;
    read_crc_once=write_crc_once=replace_on_error=cid_generation=0;
    memset(blocks,0,sizeof(blocks));busy_until=0;
    set_bits(127,126,hc?1:0);
    if(hc)set_bits(69,48,65535); /* 32 GiB */
    else { set_bits(83,80,10);set_bits(73,62,3771);set_bits(49,47,7); }
    registers[0x4800]=0xffffffff;registers[0x8040]=0xffffffff;
    assert(apx_sd_init()==0);
    assert((registers[0x8040]&6)==0&&(registers[0x8000]&4)==0);
    assert(registers[0x4800]==0xffffffff&&!(registers[0x9884]&0x40));
    assert(apx_sd_get_info()->sectors==(hc?67108864:3862528));
    assert(apx_sd_get_info()->clock_hz==24000000);
    assert(!apx_sd_set_clock(12000000)&&apx_sd_get_info()->clock_hz==12000000);
    assert(apx_sd_set_clock(100000)==APX_SD_RANGE&&apx_sd_set_clock(25000000)==APX_SD_RANGE);
    assert(!apx_sd_set_clock(24000000)&&apx_sd_get_info()->clock_hz==24000000);
}
int main(void) {
    uint8_t *ram=mmap((void *)0x021a0000,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(ram!=(void *)-1);
    for(unsigned hc=0;hc<2;++hc) {
        initialize(hc);memset(ram,0x5a,8192);
        assert(disk_status(0)==0&&disk_status(1)==STA_NOINIT);
        LBA_t sectors;assert(disk_ioctl(0,GET_SECTOR_COUNT,&sectors)==RES_OK&&sectors==apx_sd_get_info()->sectors);
        assert(apx_sd_read(123,ram+1,8)==0);
        for(unsigned s=0;s<8;++s)for(unsigned i=0;i<512;++i)assert(ram[1+s*512+i]==((123+s)*97+i)%256);
        assert(ram[0]==0x5a&&ram[4097]==0x5a&&polls);
        uint8_t original[4096];memcpy(original,ram+1,sizeof(original));
        assert(disk_write(0,ram+1,123,8)==RES_OK);assert(apx_sd_get_info()->writes==8);
        memset(ram+1,0,4096);assert(apx_sd_read(123,ram+1,8)==0);
        assert(!memcmp(ram+1,original,4096)&&ram[0]==0x5a&&ram[4097]==0x5a);
        assert(disk_ioctl(0,CTRL_SYNC,0)==RES_OK);
        assert(apx_sd_write(0,NULL,1)==APX_SD_RANGE);
        assert(apx_sd_write(0,ram,0)==APX_SD_RANGE);
        assert(apx_sd_write((uint32_t)apx_sd_get_info()->sectors-1,ram,2)==APX_SD_RANGE);
        assert(apx_sd_write(0,(void *)0x023fffff,1)==APX_SD_RANGE);
        assert(disk_write(1,ram,0,1)==RES_PARERR);
        assert(apx_sd_read((uint32_t)apx_sd_get_info()->sectors-1,ram,2)==APX_SD_RANGE);
        assert(apx_sd_read(0,ram,0)==APX_SD_RANGE);
        assert(apx_sd_read(0,(void *)0x023fffff,1)==APX_SD_RANGE);
        initialize(hc);read_crc_once=1;
        assert(!apx_sd_set_clock(12000000));assert(!apx_sd_read(0,ram,1));
        assert(apx_sd_get_info()->retries==1&&apx_sd_get_info()->errors==1&&apx_sd_get_info()->reads==1);
        assert(apx_sd_get_info()->clock_hz==12000000&&apx_sd_get_info()->last_failure_command==17);
        initialize(hc);write_crc_once=1;assert(!apx_sd_write(2,ram,1));
        assert(apx_sd_get_info()->retries==1&&apx_sd_get_info()->writes==1&&apx_sd_get_info()->errors==1);
        initialize(hc);read_crc_once=replace_on_error=1;assert(apx_sd_read(0,ram,1)==APX_SD_CARD_ERROR);
        assert(!apx_sd_get_info()->initialized&&apx_sd_get_info()->retries==1);
        initialize(hc);crc_error=1;assert(apx_sd_read(0,ram,1)==APX_SD_CARD_ERROR);assert(!apx_sd_get_info()->initialized);
        assert(apx_sd_get_info()->retries==2&&apx_sd_get_info()->errors==3);
        assert(disk_status(0)&STA_NOINIT);assert(disk_read(0,ram,0,1)==RES_NOTRDY);
        initialize(hc);data_stuck=1;assert(apx_sd_read(0,ram,1)==APX_SD_TIMEOUT);assert(!apx_sd_get_info()->initialized);
        initialize(hc);present=0;assert(apx_sd_read(0,ram,1)==APX_SD_TIMEOUT);
        present=1;assert(apx_sd_init()==0);assert(apx_sd_read(0,ram,1)==0);
        initialize(hc);write_crc_error=1;assert(apx_sd_write(2,ram,1)==APX_SD_CARD_ERROR);
        assert(!apx_sd_get_info()->initialized&&disk_write(0,ram,0,1)==RES_NOTRDY);
        initialize(hc);data_stuck=1;assert(apx_sd_write(2,ram,1)==APX_SD_TIMEOUT);
        initialize(hc);busy_stuck=1;assert(apx_sd_write(2,ram,1)==APX_SD_TIMEOUT);
        initialize(hc);status_stuck=1;assert(apx_sd_sync()==APX_SD_TIMEOUT);
        initialize(hc);r1_write_protected=1;assert(apx_sd_write(2,ram,1)==APX_SD_WRITE_PROTECTED);
        assert(apx_sd_get_info()->write_protected);
        for(unsigned bit=12;bit<=13;++bit) {
            initialize(hc);set_bits(bit,bit,1);assert(apx_sd_init()==0);
            assert(disk_status(0)==STA_PROTECT&&disk_write(0,ram,0,1)==RES_WRPRT);
        }
    }
    stuck=1;assert(apx_sd_init()==APX_SD_TIMEOUT);assert(!apx_sd_get_info()->initialized);
    initialize(0);set_bits(83,80,15);set_bits(73,62,4095);
    assert(apx_sd_init()==APX_SD_UNSUPPORTED);assert(!apx_sd_get_info()->initialized);
    initialize(1);set_bits(69,48,0x1ffff);assert(apx_sd_init()==0);
    assert(apx_sd_get_info()->sectors==134217728); /* 64 GiB SDXC */
    assert(apx_sd_read(134217727,ram,1)==0);
    set_bits(69,48,0x3fffff);assert(apx_sd_init()==0);
    assert(apx_sd_get_info()->sectors==UINT64_C(0x100000000));
    assert(apx_sd_read(UINT32_MAX,ram,1)==0); /* Last 32-bit command address */
    assert(apx_sd_write(UINT32_MAX,ram+1,1)==0); /* Last 32-bit write command address */
    puts("SD model: SDSC/SDHC RX/TX, DMA bounds, CRC/busy/status timeouts, protection, bounded read/write retry, clock/stat preservation and changed-CID refusal passed");
}
