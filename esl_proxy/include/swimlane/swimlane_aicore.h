/*
 * L2 swimlane AICore kernel hooks (aicore_kernel.cpp).
 */
#ifndef ESL_PROXY_SWIMLANE_AICORE_H
#define ESL_PROXY_SWIMLANE_AICORE_H

#include "kernel_args.h"
#include "swimlane_device.h"

#include <stdint.h>

#if ESL_PROXY_ENABLE_L2_SWIMLANE

#include "aicore.h"

__aicore__ void set_aicore_profiling_flag(uint32_t flag);
__aicore__ uint32_t get_aicore_profiling_flag(void);
__aicore__ void set_l2_swimlane_aicore_head_slot(__gm__ uint64_t *slot_ptr);
__aicore__ __gm__ L2SwimlaneActiveHead *get_l2_swimlane_aicore_head(void);

typedef struct L2SwimlaneAicoreLocalState {
    __gm__ L2SwimlaneAicoreTaskBuffer *cached_buf;
    uint32_t cached_buf_seq;
    uint32_t slot_within_buf;
} L2SwimlaneAicoreLocalState;

__aicore__ __attribute__((always_inline)) static inline void l2_swimlane_aicore_record_task(
    __gm__ L2SwimlaneActiveHead *head, L2SwimlaneAicoreLocalState *local, uint64_t task_token_raw, uint32_t reg_task_id,
    uint64_t start_time, uint64_t end_time
)
{
    dcci(head, SINGLE_CACHE_LINE);
    if (head->current_buf_seq != local->cached_buf_seq) {
        local->cached_buf_seq = head->current_buf_seq;
        local->cached_buf = (__gm__ L2SwimlaneAicoreTaskBuffer *)(uintptr_t)head->current_buf_ptr;
        local->slot_within_buf = 0;
    }
    if (local->cached_buf == NULL) {
        return;
    }

    {
        uint32_t slot = local->slot_within_buf;

        if (slot >= PLATFORM_AICORE_BUFFER_SIZE) {
            return;
        }

        {
            __gm__ L2SwimlaneAicoreTaskRecord *record = &local->cached_buf->records[slot];

            record->start_time = start_time;
            record->end_time = end_time;
            record->task_token_raw = task_token_raw;
            record->reg_task_id = reg_task_id;
            local->slot_within_buf = slot + 1;

            dcci(record, SINGLE_CACHE_LINE, CACHELINE_OUT);
            dsb((mem_dsb_t)0);
        }
    }
}

#define ESL_SWIMLANE_IS_FLAG_ON(flags) GET_PROFILING_FLAG((flags), PROFILING_FLAG_L2_SWIMLANE)
#define ESL_SWIMLANE_AICORE_RECORD_TASK(head, local, task_token_raw, reg_task_id, start_ts, end_ts) \
    l2_swimlane_aicore_record_task((head), (local), (task_token_raw), (reg_task_id), (start_ts), (end_ts))

#define ESL_SWIMLANE_AICORE_KERNEL_ENTRY(k_args, block_idx) \
    do { \
        set_aicore_profiling_flag((k_args)->enable_profiling_flag); \
        if (ESL_SWIMLANE_IS_FLAG_ON((k_args)->enable_profiling_flag) && \
            (k_args)->l2_swimlane_aicore_rotation_table != 0) { \
            __gm__ uint64_t *_esl_sw_rot = \
                (__gm__ uint64_t *)(uintptr_t)((k_args)->l2_swimlane_aicore_rotation_table); \
            set_l2_swimlane_aicore_head_slot(&_esl_sw_rot[(block_idx)]); \
        } else { \
            set_l2_swimlane_aicore_head_slot(NULL); \
        } \
    } while (0)

#define ESL_SWIMLANE_AICORE_LOCAL_STATE(var) L2SwimlaneAicoreLocalState var = {NULL, UINT32_MAX, 0}
#define ESL_SWIMLANE_AICORE_TASK_BEGIN(start_var) uint64_t start_var = get_sys_cnt_aicore()

#define ESL_SWIMLANE_AICORE_TASK_RECORD(swim_local, swim_head, exec_payload, task_id, start_time) \
    do { \
        if ((swim_head) != NULL) { \
            /* token = 编排 task_id（payload 携带），区分 qk/sf/pv/online；reg_task_id 仍作消歧 */ \
            uint64_t _esl_sw_token = (uint64_t)((exec_payload)->async_task_token); \
            ESL_SWIMLANE_AICORE_RECORD_TASK((swim_head), &(swim_local), _esl_sw_token, (task_id), (start_time), \
                                            get_sys_cnt_aicore()); \
        } \
    } while (0)

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

#define ESL_SWIMLANE_IS_FLAG_ON(flags) ((void)(flags), 0)
#define ESL_SWIMLANE_AICORE_KERNEL_ENTRY(k_args, block_idx) ((void)(k_args), (void)(block_idx))
#define ESL_SWIMLANE_AICORE_LOCAL_STATE(var) int var = 0
#define ESL_SWIMLANE_AICORE_TASK_BEGIN(start_var) uint64_t start_var = 0
#define ESL_SWIMLANE_AICORE_TASK_RECORD(swim_local, swim_head, exec_payload, task_id, start_time) \
    ((void)(swim_local), (void)(swim_head), (void)(exec_payload), (void)(task_id), (void)(start_time))

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_SWIMLANE_AICORE_H */
