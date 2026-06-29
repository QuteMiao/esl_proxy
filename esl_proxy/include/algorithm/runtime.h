/*
 * Minimal Runtime / Handshake layout (algorithm layer).
 * Public field order matches simpler tensormap_and_ringbuffer Runtime
 * through func_id_to_addr_ for GM binary compatibility.
 */
#ifndef ESL_PROXY_ALGORITHM_RUNTIME_H
#define ESL_PROXY_ALGORITHM_RUNTIME_H

#include <stdint.h>

#include "worker_map.h"

#ifndef RUNTIME_MAX_WORKER
#define RUNTIME_MAX_WORKER ESL_PROXY_HOST_WORKER_COUNT
#endif
#ifndef RUNTIME_MAX_FUNC_ID
#define RUNTIME_MAX_FUNC_ID 1024
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EslHandshake {
    volatile uint32_t aicpu_ready;
    volatile uint32_t aicore_done;
    volatile uint64_t task;
    volatile int32_t core_type;
    volatile uint32_t physical_core_id;
    volatile uint32_t aicpu_regs_ready;
    volatile uint32_t aicore_regs_ready;
} __attribute__((aligned(64))) EslHandshake;

typedef struct EslRuntime {
    EslHandshake workers[RUNTIME_MAX_WORKER];
    int worker_count;
    int aicpu_thread_num;
    int ready_queue_shards;
    uint64_t task_window_size;
    uint64_t heap_size;
    uint64_t dep_pool_size;
    uint64_t func_id_to_addr_[RUNTIME_MAX_FUNC_ID];
    uint8_t orch_to_sched;
    /* AICPU CPU 亲和性门控(对齐 simpler):host 探测 OCCUPY 掩码后填入允许的控制 CPU,
     * AICPU 侧 gate filter 据此从 launch_count 个线程里选出 allowed_count 个钉到这些核运行,
     * 其余线程退出。防止 AICPU 线程漂移/争用拖垮 AICore 握手。 */
    int32_t aicpu_allowed_cpus[16];
    int32_t aicpu_allowed_cpu_count;
    int32_t aicpu_launch_count;
} EslRuntime;

/* PTO2 512B per-core dispatch payload (simpler: PTO2DispatchPayload). */
typedef struct EslDispatchPayload {
    uint64_t function_bin_addr;                 /* 8B */
    uint64_t args[50];                          /* 400B (simpler: args[50]) */

    /* LocalContext — 48B (simpler: intrinsic.h LocalContext) */
    int32_t local_block_idx;                    /* 4B */
    int32_t local_block_num;                    /* 4B */
    /* AsyncCtx — 40B (simpler: intrinsic.h AsyncCtx; esl uses uint64 placeholders) */
    uint64_t async_completion_count;            /* 8B */
    uint64_t async_completion_error_code;       /* 8B */
    uint64_t async_completion_entries;          /* 8B */
    uint32_t async_completion_capacity;         /* 4B */
    uint64_t async_task_token;                  /* 8B */

    /* GlobalContext — 4B (simpler: intrinsic.h GlobalContext) */
    int32_t global_sub_block_id;                /* 4B */

    volatile uint32_t not_ready;                /* 4B */
    uint8_t reserved_payload_abi_pad[4];        /* 4B */
} __attribute__((aligned(64))) EslDispatchPayload; /* 512B */

typedef struct EslPublishHandle {
    uint64_t reg_addr;
    uint32_t reg_task_id;
} EslPublishHandle;

void esl_init_global_context(EslRuntime *runtime);
EslPublishHandle esl_prepare_subtask_to_core(EslRuntime *runtime, int core, uint16_t task_id,
                                             uint32_t block_idx);
void esl_publish_subtask_to_core(EslPublishHandle handle);

int32_t esl_aicpu_execute(EslRuntime *runtime);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ALGORITHM_RUNTIME_H */
