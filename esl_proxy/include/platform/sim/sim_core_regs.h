/*
 * sim_core_regs.h — per-HAL-slot register backing for sim AICore worker threads.
 */
#ifndef ESL_PROXY_SIM_CORE_REGS_H
#define ESL_PROXY_SIM_CORE_REGS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host (dispatch/shutdown) and AICore worker threads share each slot; volatile
 * for cross-thread visibility in sim (no MMIO). */
typedef struct SimCoreReg {
    volatile uint32_t data_main_base;
    volatile uint32_t cond;
    volatile uint8_t exited;
} SimCoreReg;

void sim_core_regs_init(void);
SimCoreReg *sim_core_reg_at(uint64_t reg_base_addr);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_SIM_CORE_REGS_H */
