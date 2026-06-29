/*
 * fake_kernel.h — fake kernel busy-wait (platform time via esl_aicore_now_ns).
 */
#ifndef ESL_PROXY_FAKE_KERNEL_H
#define ESL_PROXY_FAKE_KERNEL_H

#include "aicore.h"
#include <stdint.h>

/* Busy-wait for duration_ns ± jitter derived from start & jitter_mask (§4.2). */
__aicore__ __attribute__((always_inline)) static inline void fake_kernel_run(uint64_t duration_ns, uint64_t jitter_mask) {
    uint64_t start = esl_aicore_now_ns();
    int64_t wait_ns = (int64_t)duration_ns + (int64_t)(start & jitter_mask) - (int64_t)((jitter_mask + 1U) / 2U);
    uint64_t end = start + (uint64_t)wait_ns;

    while (esl_aicore_now_ns() < end) {
        /* spin */
    }
}

#endif /* ESL_PROXY_FAKE_KERNEL_H */
