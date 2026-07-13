/*
 * aicore.h — host-side AICore HAL for sim worker threads (same filename as onboard/aicore.h).
 */
#ifndef ESL_PROXY_SIM_AICORE_H
#define ESL_PROXY_SIM_AICORE_H

#include "platform_config.h"
#include "sim_core_regs.h"

#include <stdint.h>
#include <time.h>

#ifndef __gm__
#define __gm__
#endif

#ifndef __aicore__
#define __aicore__
#endif

#define SPIN_WAIT_HINT() ((void)0)
#define OUT_OF_ORDER_STORE_BARRIER() __asm__ __volatile__("" ::: "memory")
#define OUT_OF_ORDER_LOAD_BARRIER() __asm__ __volatile__("" ::: "memory")
#define OUT_OF_ORDER_FULL_BARRIER() __asm__ __volatile__("" ::: "memory")

#define SINGLE_CACHE_LINE 0
#define ENTIRE_DATA_CACHE 1
#define CACHELINE_OUT 2

#define dcci(addr, ...) ((void)(addr))

#ifndef dsb
#define dsb(x) ((void)0)
#endif

#ifndef mem_dsb_t
typedef int mem_dsb_t;
#endif

#ifdef __cplusplus
extern "C" {
#endif

void sim_aicore_tls_set(SimCoreReg *reg, uint32_t physical_core_id);

#ifdef __cplusplus
}
#endif

static inline SimCoreReg *sim_aicore_tls_reg(void)
{
    extern __thread SimCoreReg *g_sim_aicore_tls_reg;
    extern __thread uint32_t g_sim_aicore_tls_phys;

    (void)g_sim_aicore_tls_phys;
    return g_sim_aicore_tls_reg;
}

static inline uint64_t read_reg(RegId reg)
{
    SimCoreReg *cr = sim_aicore_tls_reg();

    if (cr == NULL) {
        return 0;
    }
    OUT_OF_ORDER_LOAD_BARRIER();
    if (reg == REG_ID_DATA_MAIN_BASE) {
        /* Host shutdown sets exited before doorbell; treat as EXIT even if a
         * concurrent deinit reset races the raw data_main_base field. */
        if (cr->exited) {
            return (uint64_t)AICORE_EXIT_SIGNAL;
        }
        return (uint64_t)cr->data_main_base;
    }
    if (reg == REG_ID_COND) {
        return (uint64_t)cr->cond;
    }
    return 0;
}

static inline void write_reg(RegId reg, uint64_t value)
{
    SimCoreReg *cr = sim_aicore_tls_reg();

    if (cr == NULL) {
        return;
    }
    if (reg == REG_ID_DATA_MAIN_BASE) {
        if (value == (uint64_t)AICORE_EXIT_SIGNAL) {
            cr->exited = 1;
        }
        cr->data_main_base = (uint32_t)value;
        OUT_OF_ORDER_STORE_BARRIER();
        return;
    }
    if (reg == REG_ID_COND) {
        cr->cond = (uint32_t)value;
        OUT_OF_ORDER_STORE_BARRIER();
    }
}

static inline uint32_t get_physical_core_id(void)
{
    extern __thread uint32_t g_sim_aicore_tls_phys;

    return g_sim_aicore_tls_phys;
}

static inline uint64_t get_sys_cnt_aicore(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * ESL_ONBOARD_SYS_CNT_FREQ +
           (uint64_t)ts.tv_nsec * ESL_ONBOARD_SYS_CNT_FREQ / 1000000000ULL;
}

static inline uint64_t esl_aicore_now_ns(void)
{
    return get_sys_cnt_aicore() * 1000000000ULL / ESL_ONBOARD_SYS_CNT_FREQ;
}

static inline uint32_t read_dmb_high32(void)
{
    SimCoreReg *cr = sim_aicore_tls_reg();

    if (cr == NULL) {
        return 0;
    }
    OUT_OF_ORDER_LOAD_BARRIER();
    return cr->data_main_base_high;
}

#endif /* ESL_PROXY_SIM_AICORE_H */
