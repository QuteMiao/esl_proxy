/*
 * Minimal Runtime / Handshake layout (algorithm layer).
 * Public field order matches simpler tensormap_and_ringbuffer Runtime
 * through func_id_to_addr_ for GM binary compatibility.
 */
#ifndef ESL_PROXY_ALGORITHM_RUNTIME_H
#define ESL_PROXY_ALGORITHM_RUNTIME_H

#include <stdint.h>

#include "worker_map.h"
#include "task.h"

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
} EslRuntime;

/* GM per-core dual-slot payload IS struct task_desc (PR #13: g_basic_buf=payload,
 * no separate EslDispatchPayload). Slot stride = sizeof(struct task_desc).
 * not_ready for gated publish is stored in scalar[31] of the GM copy. */
#define ESL_GM_TASK_NOT_READY_SCALAR 31

typedef struct EslPublishHandle {
    uint64_t reg_addr;
    uint32_t reg_task_id;
} EslPublishHandle;

void esl_init_global_context(EslRuntime *runtime);
EslPublishHandle esl_prepare_subtask_to_core(EslRuntime *runtime, int core, uint16_t task_id,
                                             uint32_t block_idx);
EslPublishHandle esl_prepare_subtask_to_core_gated(EslRuntime *runtime, int core, uint16_t task_id,
                                                   uint32_t block_idx, int not_ready);
void esl_publish_subtask_to_core(EslPublishHandle handle);

int32_t esl_aicpu_execute(EslRuntime *runtime);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ALGORITHM_RUNTIME_H */
