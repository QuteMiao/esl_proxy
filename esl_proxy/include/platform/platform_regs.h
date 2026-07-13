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
int platform_reg_task_acked(uint64_t reg_base_addr, uint32_t reg_task);
void platform_reg_task_ack(uint64_t reg_base_addr, uint32_t reg_task);
/* True only when the AICore worker wrote COND == AICORE_EXITED_VALUE (raw field).
 * Must NOT treat host Phase-1 exited=1 as ack — that caused shutdown to write IDLE
 * before the worker left aicore_execute(). */
int platform_reg_worker_exit_acked(uint64_t reg_base_addr);
/* Single consistent read of the COND register (raw 32-bit: [bit31 state | low31 id]).
 * For double-buffer completion inference where id AND state must come from one load. */
uint32_t platform_reg_cond_raw(uint64_t reg_base_addr);

/* Resolve the COND register's address ONCE (reg_base + reg_offset(REG_ID_COND)) so
 * the hot dispatch poll can read it directly — skipping the reg_offset switch + base
 * addition on every poll (mirrors simpler's precomputed core.cond_ptr). The caller
 * issues its own OUT_OF_ORDER_LOAD_BARRIER() after the volatile load. */
volatile uint32_t *platform_reg_cond_ptr(uint64_t reg_base_addr);

/* Speculative release doorbell: 64-bit DATA_MAIN_BASE write (high=low=token). */
void platform_ring_spec_doorbell(uint64_t reg_base_addr, uint32_t token);

void cache_invalidate_range(const void *addr, size_t size);
void cache_flush_range(const void *addr, size_t size);

/* Batched invalidate: cache_civac_lines() per region (no barrier) + ONE cache_civac_barrier(). */
void cache_civac_lines(const void *addr, size_t size);
void cache_civac_barrier(void);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_REGS_H */
