/*
 * platform_config.h — platform configuration shared by BOTH the onboard and sim
 * backends: dimensions, profiling flags, register-layout constants, RegId, task-id
 * encoding, dispatch payload layout, and small inline helpers.
 *
 * Single source of truth. Onboard-only knobs (CANN host timeouts) live in
 * include/platform/onboard/onboard_config.h, which #includes this header.
 */
#ifndef ESL_PROXY_PLATFORM_CONFIG_H
#define ESL_PROXY_PLATFORM_CONFIG_H

#include <stdint.h>

#include "worker_map.h"

/* Scheduler-lane count. Authoritative value comes from -DESL_LANE_CNT on the
 * command line (kept in sync with conf.h); this fallback only applies when this
 * header is compiled standalone. Do NOT #include conf.h here — that would invert
 * the platform<-algorithm layering. */
#ifndef ESL_LANE_CNT
#define ESL_LANE_CNT 1
#endif

#ifndef ESL_PROXY_ENABLE_L2_SWIMLANE
#define ESL_PROXY_ENABLE_L2_SWIMLANE 0
#endif

#define PLATFORM_MAX_BLOCKDIM ESL_PROXY_WORKER_BLOCK_DIM
#define PLATFORM_CORES_PER_BLOCKDIM 3
#define PLATFORM_AIV_CORES_PER_BLOCKDIM ESL_PROXY_AIV_LANES_PER_BLOCK
#define PLATFORM_MAX_AICPU_THREADS 4

#define PLATFORM_PROF_SYS_CNT_FREQ 50000000ULL
#define ESL_ONBOARD_SYS_CNT_FREQ PLATFORM_PROF_SYS_CNT_FREQ

#define PROFILING_FLAG_L2_SWIMLANE (1u << 1)
#define GET_PROFILING_FLAG(flags, bit) ((((uint32_t)(flags)) & ((uint32_t)(bit))) != 0u)

#ifdef __cplusplus
extern "C" {
#endif

/* --- esl_proxy bring-up --- */
/* Overlapped model only (orchestrator-first folded model is not included
 * in this branch). N cutter + N dispatch + 1 standalone orch =
 * 2*ESL_LANE_CNT + 1 threads. Keeping this <= PLATFORM_MAX_AICPU_THREADS(4)
 * keeps all threads inside one a3 AICPU cluster (ESL_AICPU_CPUS_PER_CLUSTER=4);
 * the overlapped model therefore fits only a single lane onboard. */
#define ESL_PROXY_AICPU_THREAD_NUM (2 * ESL_LANE_CNT + 1)
/* Swimlane semantic tags (NOT thread indices). */
#define ESL_AICPU_ROLE_CUTTER 0
#define ESL_AICPU_ROLE_DISPATCH 1
#define ESL_AICPU_ROLE_ORCH (ESL_PROXY_AICPU_THREAD_NUM - 1)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(ESL_PROXY_AICPU_THREAD_NUM <= PLATFORM_MAX_AICPU_THREADS,
               "aicpu threads must fit one AICPU cluster / static thread arrays");
#endif
#define ESL_PROXY_ONBOARD_BLOCK_DIM ESL_PROXY_WORKER_BLOCK_DIM
#define ESL_PROXY_ONBOARD_AIC_COUNT ESL_PROXY_ONBOARD_BLOCK_DIM
#define ESL_PROXY_ONBOARD_AIV_COUNT (ESL_PROXY_ONBOARD_BLOCK_DIM * PLATFORM_AIV_CORES_PER_BLOCKDIM)
#define ESL_PROXY_ONBOARD_WORKER_COUNT (ESL_PROXY_ONBOARD_AIC_COUNT + ESL_PROXY_ONBOARD_AIV_COUNT)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(ESL_PROXY_ONBOARD_WORKER_COUNT == ESL_PROXY_HOST_WORKER_COUNT,
               "onboard and sim worker topology must match worker_map.h");
#endif

#define REG_SPR_DATA_MAIN_BASE_OFFSET 0xA0U
#define REG_SPR_COND_OFFSET 0x4C8U
#define REG_SPR_FAST_PATH_ENABLE_OFFSET 0x18U
#define REG_SPR_FAST_PATH_OPEN 0xEU
#define REG_SPR_FAST_PATH_CLOSE 0xFU
#define AICORE_EXIT_SIGNAL 0x7FFFFFF0U
#define AICORE_COREID_MASK 0x0FFFU

#define PLATFORM_SUB_CORES_PER_AICORE PLATFORM_CORES_PER_BLOCKDIM
#define DAV_2201_PLATFORM_MAX_PHYSICAL_CORES 25U
#define ESL_PROXY_PLATFORM_HAL_REG_SLOTS \
    (DAV_2201_PLATFORM_MAX_PHYSICAL_CORES * PLATFORM_SUB_CORES_PER_AICORE)

#define TASK_ID_MASK 0x7FFFFFFFU
#define TASK_STATE_MASK 0x80000000U
#define TASK_ACK_STATE 0U
#define TASK_FIN_STATE 1U

#define EXTRACT_TASK_ID(regval) ((int)((regval) & TASK_ID_MASK))
#define EXTRACT_TASK_STATE(regval) ((int)(((regval) & TASK_STATE_MASK) >> 31))
#define MAKE_ACK_VALUE(task_id) ((uint64_t)((task_id) & TASK_ID_MASK))
#define MAKE_FIN_VALUE(task_id) ((uint64_t)(((task_id) & TASK_ID_MASK) | TASK_STATE_MASK))

#define AICORE_IDLE_TASK_ID 0x7FFFFFFFU
#define AICORE_EXIT_TASK_ID 0x7FFFFFFEU
#define AICPU_IDLE_TASK_ID 0x7FFFFFFDU
#define AICORE_IDLE_VALUE MAKE_FIN_VALUE(AICORE_IDLE_TASK_ID)
#define AICORE_EXITED_VALUE MAKE_FIN_VALUE(AICORE_EXIT_TASK_ID)

#define PLATFORM_AICORE_MAP_BUFF_LEN 2U

typedef enum {
    REG_ID_DATA_MAIN_BASE = 0,
    REG_ID_COND = 1,
    REG_ID_FAST_PATH_ENABLE = 2,
} RegId;

static inline uint64_t esl_duration_ns_to_sys_cnt(uint64_t duration_ns)
{
    return duration_ns * ESL_ONBOARD_SYS_CNT_FREQ / 1000000000ULL;
}

static inline int esl_worker_to_hal_reg_index(int worker_id)
{
    if (worker_id < 0 || worker_id >= ESL_PROXY_ONBOARD_WORKER_COUNT) {
        return -1;
    }
    if (worker_id < ESL_PROXY_ONBOARD_AIC_COUNT) {
        return worker_id;
    }
    return worker_id + 1;
}

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_PLATFORM_CONFIG_H */
