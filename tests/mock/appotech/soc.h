#ifndef MOCK_APPOTECH_SOC_H
#define MOCK_APPOTECH_SOC_H
#include <stdint.h>
#include <stddef.h>
#define APX_RAM_BASE 0x02000000u
#define APX_RAM_END 0x02400000u
#define APX_RUNTIME_BASE 0x02180000u
#define APX_RUNTIME_END 0x021a0000u
#define APX_STACK_BASE 0x02200000u
#define APX_STACK_END 0x02204000u
void apx_cache_flush_range(const void *start,const void *end);
_Noreturn void apx_bootrom(void);
uint32_t apx_spr_read(uint32_t spr);
void apx_spr_write(uint32_t spr,uint32_t value);
uint32_t apx_ticks(void);
void apx_maintenance(void);
void apx_delay_ms(uint32_t milliseconds);
int apx_dma_clean(const void *buffer,size_t length);
int apx_dma_invalidate(void *buffer,size_t length);
uint32_t apx_mmio_read32(uintptr_t address);
uint8_t apx_mmio_read8(uintptr_t address);
static inline void apx_cpu_relax(void) {}
static inline int apx_deadline_expired(uint32_t deadline) { return (int32_t)(apx_ticks()-deadline)>=0; }
#endif
