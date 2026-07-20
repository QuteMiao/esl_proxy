/*
 * aicore_executor.c — AICore handshake + dispatch loop (algorithm layer).
 *
 * Platform entry (CANN KERNEL_ENTRY) lives in platform/a2a3/aicore_entry.cpp.
 * Device HAL via aicore.h (sim or onboard, selected by -I path).
 *
 * GM payload slots are struct task_desc (same layout as g_basic_buf).
 */
#include "aicore_executor.h"

#include "fake_kernel.h"
#include "task.h"

#include <stdint.h>

/* PR #13: schedule-overhead timestamps — record in memory, batch-dump on EXIT. */
#define ESL_AICORE_SCHED_TS_CAP 128
typedef struct {
    uint32_t task_id;
    uint64_t t_ack_ns;
    uint64_t t_fin_ns;
} EslAicoreSchedTs;

__aicore__ __attribute__((always_inline)) static void execute_task(__gm__ struct task_desc *desc)
{
    if (desc == NULL || desc->kernel == NULL) {
        return;
    }

    fake_kernel_run((uint64_t)desc->duration, (uint64_t)desc->jitter_mask);
    OUT_OF_ORDER_STORE_BARRIER();
}

__aicore__ __attribute__((always_inline)) static void esl_sched_ts_dump(const EslAicoreSchedTs *buf,
                                                                         uint32_t n, int block_idx,
                                                                         __gm__ EslRuntime *runtime)
{
    uint32_t i;
    uint64_t sum_delta = 0;
    for (i = 0; i < n; i++) {
        sum_delta += buf[i].t_fin_ns - buf[i].t_ack_ns;
    }
    if (block_idx == 0) {
        runtime->task_window_size = (uint64_t)n;
        runtime->heap_size = sum_delta;
        dcci(&runtime->task_window_size, SINGLE_CACHE_LINE, CACHELINE_OUT);
        dcci(&runtime->heap_size, SINGLE_CACHE_LINE, CACHELINE_OUT);
    }
}

__aicore__ __attribute__((weak)) void aicore_execute(__gm__ EslRuntime *runtime, int block_idx,
                                                      CoreType worker_core_type)
{
    __gm__ EslHandshake *my_hank = (__gm__ EslHandshake *)(&runtime->workers[block_idx]);
    EslAicoreSchedTs sched_ts[ESL_AICORE_SCHED_TS_CAP];
    uint32_t sched_ts_n = 0;

    while (my_hank->aicpu_ready == 0) {
        dcci(my_hank, SINGLE_CACHE_LINE);
        SPIN_WAIT_HINT();
    }

    my_hank->physical_core_id = get_physical_core_id();
    OUT_OF_ORDER_STORE_BARRIER();
    my_hank->aicore_regs_ready = 1;
    dcci(&my_hank->aicore_regs_ready, SINGLE_CACHE_LINE, CACHELINE_OUT);
    while (my_hank->aicpu_regs_ready == 0) {
        dcci(&my_hank->aicpu_regs_ready, SINGLE_CACHE_LINE);
        SPIN_WAIT_HINT();
    }

    write_reg(REG_ID_COND, AICORE_IDLE_VALUE);

    my_hank->core_type = (int32_t)worker_core_type;
    OUT_OF_ORDER_STORE_BARRIER();
    my_hank->aicore_done = (uint32_t)(block_idx + 1);
    dcci(my_hank, SINGLE_CACHE_LINE, CACHELINE_OUT);

    __gm__ struct task_desc *payload_base =
        (__gm__ struct task_desc *)(uintptr_t)my_hank->task;

    uint32_t reg_val = AICPU_IDLE_TASK_ID;
    uint32_t last_reg_val = AICPU_IDLE_TASK_ID;

    while (1) {
        reg_val = (uint32_t)read_reg(REG_ID_DATA_MAIN_BASE);
        if (reg_val == AICORE_EXIT_SIGNAL) {
            write_reg(REG_ID_COND, AICORE_EXITED_VALUE);
            break;
        }

        if (reg_val == AICPU_IDLE_TASK_ID || reg_val == last_reg_val) {
            SPIN_WAIT_HINT();
            continue;
        }

        {
            uint32_t task_id = reg_val;
            __gm__ struct task_desc *exec_payload = payload_base + (task_id & 1u);
            uint64_t t_ack_ns;
            uint64_t t_fin_ns;

            dcci(exec_payload, ENTIRE_DATA_CACHE);

            if (exec_payload->scalar[ESL_GM_TASK_NOT_READY_SCALAR] != 0) {
                while (1) {
                    if (read_dmb_high32() == task_id) {
                        if ((uint32_t)read_reg(REG_ID_DATA_MAIN_BASE) == AICORE_EXIT_SIGNAL) {
                            write_reg(REG_ID_COND, AICORE_EXITED_VALUE);
                            esl_sched_ts_dump(sched_ts, sched_ts_n, block_idx, runtime);
                            return;
                        }
                        break;
                    }
                    if ((uint32_t)read_reg(REG_ID_DATA_MAIN_BASE) == AICORE_EXIT_SIGNAL) {
                        write_reg(REG_ID_COND, AICORE_EXITED_VALUE);
                        esl_sched_ts_dump(sched_ts, sched_ts_n, block_idx, runtime);
                        return;
                    }
                    SPIN_WAIT_HINT();
                }
            }

            t_ack_ns = esl_aicore_now_ns();
            write_reg(REG_ID_COND, MAKE_ACK_VALUE(task_id));

            execute_task(exec_payload);

            last_reg_val = reg_val;
            write_reg(REG_ID_COND, MAKE_FIN_VALUE(task_id));
            t_fin_ns = esl_aicore_now_ns();

            if (sched_ts_n < ESL_AICORE_SCHED_TS_CAP) {
                sched_ts[sched_ts_n].task_id = task_id;
                sched_ts[sched_ts_n].t_ack_ns = t_ack_ns;
                sched_ts[sched_ts_n].t_fin_ns = t_fin_ns;
                sched_ts_n++;
            }
        }
    }

    esl_sched_ts_dump(sched_ts, sched_ts_n, block_idx, runtime);
    dcci(my_hank, SINGLE_CACHE_LINE, CACHELINE_OUT);
}
