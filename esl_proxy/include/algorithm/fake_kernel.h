/*
 * fake_kernel.h — fake kernel busy-wait (aligned with fake_kernel.cpp SYS_CNT loop).
 *
 * Keeps duration_ns ± jitter_mask; while body must update cur/dur (non-empty).
 */
#ifndef ESL_PROXY_FAKE_KERNEL_H
#define ESL_PROXY_FAKE_KERNEL_H

#include "aicore.h"
#include <stdint.h>

#ifdef __CCE_KT_TEST__
/* host-side KT test: __aicore__ already empty via aicore.h */
#else
/* onboard CCE: use platform get_sys_cnt (SYS_CNT); sim overrides in platform/sim/aicore.h */
#endif

/* Busy-wait for duration_ns ± jitter from start & jitter_mask, in SYS_CNT ticks. */
__aicore__ __attribute__((always_inline)) static inline void fake_kernel_run(uint64_t duration_ns,
                                                                              uint64_t jitter_mask)
{
    uint64_t start = get_sys_cnt_aicore();
    int64_t wait_ns = (int64_t)duration_ns + (int64_t)(start & jitter_mask) -
                      (int64_t)((jitter_mask + 1U) / 2U);
    uint64_t wait_ticks;
    uint64_t cur;
    uint64_t dur;

    if (wait_ns < 0) {
        wait_ns = 0;
    }
    wait_ticks = (uint64_t)wait_ns * (uint64_t)ESL_ONBOARD_SYS_CNT_FREQ / 1000000000ULL;

    cur = start;
    dur = 0;
    while (dur < wait_ticks) {
        cur = get_sys_cnt_aicore();
        dur = cur - start;
    }
}

#endif /* ESL_PROXY_FAKE_KERNEL_H */
