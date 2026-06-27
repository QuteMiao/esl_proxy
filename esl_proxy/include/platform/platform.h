/*
 * platform.h — unified HAL interface for algorithm code.
 * Host sim links platform/sim sources; onboard links platform/onboard sources.
 */
#ifndef ESL_PROXY_PLATFORM_H
#define ESL_PROXY_PLATFORM_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLATFORM_HOST_WORKER_COUNT 72
#define PLATFORM_WORKER_BLOCK_DIM 24

#define ESL_AICPU_ROLE_CUTTER 0
#define ESL_AICPU_ROLE_DISPATCH 1
#ifndef ESL_AICPU_ROLE_ORCH
#define ESL_AICPU_ROLE_ORCH 2
#endif

#define ESL_DEVICE_WALL_SLOTS 24

void platform_main_log_vwrite(int line, const char *fmt, va_list args);

/* Single GM cache-maintenance primitive used by algorithm-layer sched snapshot sync.
 * sim backend: no-op + compiler barrier; onboard backend: dc civac (clean+invalidate).
 * Caller passes a 64B-aligned base and a 64B-multiple size. The two legacy names are
 * aliases; civac subsumes the old cvac clean-only flush. */
#ifndef cache_invalidate_range
void cache_civac_range(const void *addr, size_t size);
#define cache_invalidate_range(addr, size) cache_civac_range((addr), (size))
#define cache_flush_range(addr, size)      cache_civac_range((addr), (size))
/* Batched: cache_civac_lines() per region (no barrier) + one cache_civac_barrier(). */
void cache_civac_lines(const void *addr, size_t size);
void cache_civac_barrier(void);
#endif

/* Dispatch loop exit: publish final scheduler stats (onboard writes device_wall; sim: no-op). */
void platform_stats_publish(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt,
                            uint64_t commit, uint64_t ready_cube, uint64_t ready_vec,
                            uint64_t min_uncomplete, uint64_t elapsed_ns);

/* Sim: pre-fill handshake ack fields + fake reg table (no real AICore).
 * Onboard: no-op — real AICore sets fields in aicore_executor. */
void platform_handshake_aicore_bootstrap(EslRuntime *runtime);

int esl_platform_init(EslRuntime *runtime);
void esl_platform_shutdown(EslRuntime *runtime);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_H */
