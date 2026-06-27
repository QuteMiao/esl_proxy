/*
 * platform_regs.h — platform register access + cache operations (shared interface).
 *
 * Extern declarations shared by BOTH backends; the definitions are backend-specific:
 *   onboard → src/platform/onboard/npu_hal.c (real MMIO + aarch64 cache maintenance)
 *   sim     → src/platform/sim/platform_regs.c (host stubs, no MMIO)
 */
#ifndef ESL_PROXY_PLATFORM_REGS_H
#define ESL_PROXY_PLATFORM_REGS_H

#include <stddef.h>
#include <stdint.h>

#include "platform_config.h" /* RegId */

#ifdef __cplusplus
extern "C" {
#endif

void set_platform_regs(uint64_t regs);
uint64_t get_platform_regs(void);

uint64_t read_reg(uint64_t reg_base_addr, RegId reg);
void write_reg(uint64_t reg_base_addr, RegId reg, uint64_t value);
void platform_init_aicore_regs(uint64_t reg_addr);

int platform_reg_task_finished(uint64_t reg_base_addr, uint32_t reg_task);
void platform_reg_task_ack(uint64_t reg_base_addr, uint32_t reg_task);

/* Single GM cache-maintenance primitive (clean+invalidate, dc civac). Caller must
 * pass a 64B-aligned base and a 64B-multiple size. The two legacy names are kept as
 * aliases so existing call sites read naturally; civac subsumes cvac's clean-only. */
void cache_civac_range(const void *addr, size_t size);
#define cache_invalidate_range(addr, size) cache_civac_range((addr), (size))
#define cache_flush_range(addr, size)      cache_civac_range((addr), (size))

/* Batched form: issue dc civac over N regions with cache_civac_lines(), then ONE
 * cache_civac_barrier() — replaces N separate dsb sy with a single trailing one. */
void cache_civac_lines(const void *addr, size_t size);
void cache_civac_barrier(void);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_REGS_H */
