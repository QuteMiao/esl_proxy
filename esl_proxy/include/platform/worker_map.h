/*
 * worker_map.h — single source of the dispatch worker topology and the
 * logical-core → physical-worker mapping, shared by BOTH the host CPU
 * simulator and the onboard/AICPU build.
 *
 * Topology: block_dim=24 → 24 AIC + 48 AIV = 72 workers (1 AIC + 2 AIV per
 * block). platform.h / platform_config.h / runtime.h alias these names;
 * do not duplicate literal 24 / 72 elsewhere.
 *
 * esl_pick_phys_worker is a static inline (no separate .o, no duplicate symbol)
 * and uses the __atomic builtin rather than C11 _Atomic so it stays valid when
 * this header is pulled into C++ TUs via onboard_config.h.
 */
#ifndef ESL_PROXY_WORKER_MAP_H
#define ESL_PROXY_WORKER_MAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESL_PROXY_WORKER_BLOCK_DIM 24
#define ESL_PROXY_AIV_LANES_PER_BLOCK 2
#define ESL_PROXY_HOST_WORKER_COUNT \
    (ESL_PROXY_WORKER_BLOCK_DIM + ESL_PROXY_WORKER_BLOCK_DIM * ESL_PROXY_AIV_LANES_PER_BLOCK)

/* exe_type: TASK_TYPE_CUBE (0) or TASK_TYPE_VECTOR (1). core: 0..block_dim-1.
 * CUBE maps to its own core; VECTOR round-robins across the core's AIV lanes.
 * Returns the physical worker index (0..71), or -1 if core is out of range. */
static inline int esl_pick_phys_worker(int core, int exe_type)
{
    static uint32_t s_aiv_lane_pick[ESL_PROXY_WORKER_BLOCK_DIM];
    if (core < 0 || core >= ESL_PROXY_WORKER_BLOCK_DIM) {
        return -1;
    }
    if (exe_type == 0) {
        return core;
    }
    {
        const uint32_t lane =
            __atomic_fetch_add(&s_aiv_lane_pick[core], 1U, __ATOMIC_RELAXED) %
            (uint32_t)ESL_PROXY_AIV_LANES_PER_BLOCK;
        return ESL_PROXY_WORKER_BLOCK_DIM + core * ESL_PROXY_AIV_LANES_PER_BLOCK + (int)lane;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_WORKER_MAP_H */
