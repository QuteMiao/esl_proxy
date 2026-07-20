/*
 * platform.h — unified HAL interface for algorithm code.
 * Host Fake Return uses algorithm/ only; a2a3 links platform/a2a3 sources.
 *
 * Topology SSOT: worker_map.h (ESL_PROXY_*). This header only adds HAL aliases
 * and init/cache/stats entry points. AICPU role IDs live in platform_config.h.
 */
#ifndef ESL_PROXY_PLATFORM_H
#define ESL_PROXY_PLATFORM_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime.h"
#include "worker_map.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLATFORM_WORKER_BLOCK_DIM ESL_PROXY_WORKER_BLOCK_DIM
#define PLATFORM_HOST_WORKER_COUNT ESL_PROXY_HOST_WORKER_COUNT

#define ESL_DEVICE_WALL_SLOTS 40

static inline int platform_pick_phys_worker(int core, int exe_type)
{
    return esl_pick_phys_worker(core, exe_type);
}

void platform_main_log_vwrite(int line, const char *fmt, va_list args);

/* Cache primitives used directly by algorithm-layer sched snapshot sync.
 * sim backend: no-op + compiler barrier; onboard backend: dc civac / dc cvac. */
void cache_invalidate_range(const void *addr, size_t size);
void cache_flush_range(const void *addr, size_t size);
/* Batched invalidate: cache_civac_lines() per region (no barrier) + ONE cache_civac_barrier(). */
void cache_civac_lines(const void *addr, size_t size);
void cache_civac_barrier(void);

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
