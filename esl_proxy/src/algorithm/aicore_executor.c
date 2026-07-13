/*
 * aicore_executor.c — AICore handshake + dispatch loop (algorithm layer).
 *
 * Platform entry (CANN KERNEL_ENTRY) lives in platform/onboard/aicore_entry.cpp.
 * Device HAL via aicore.h (sim or onboard, selected by -I path).
 */
#include "aicore_executor.h"

#include "fake_kernel.h"
#include "swimlane_aicore.h"

#include <stdint.h>

__aicore__ __attribute__((always_inline)) static void execute_task(__gm__ EslDispatchPayload *payload)
{
    if (payload == NULL || payload->function_bin_addr == 0U) {
        return;
    }

    fake_kernel_run(payload->args[0], payload->args[1]);
    OUT_OF_ORDER_STORE_BARRIER();
}

__aicore__ __attribute__((weak)) void aicore_execute(__gm__ EslRuntime *runtime, int block_idx, CoreType worker_core_type,
                                      uint32_t profiling_flag, uint64_t rotation_table)
{
    __gm__ EslHandshake *my_hank = (__gm__ EslHandshake *)(&runtime->workers[block_idx]);

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

    __gm__ EslDispatchPayload *payload_base =
        (__gm__ EslDispatchPayload *)(uintptr_t)my_hank->task;

    uint32_t reg_val = AICPU_IDLE_TASK_ID;
    uint32_t last_reg_val = AICPU_IDLE_TASK_ID;
    ESL_SWIMLANE_AICORE_LOCAL_STATE(swim_local);
    __gm__ L2SwimlaneActiveHead *l2_swimlane_head = NULL;
    if (ESL_SWIMLANE_IS_FLAG_ON(profiling_flag) && rotation_table != 0U) {
        __gm__ uint64_t *head_slot = (__gm__ uint64_t *)(uintptr_t)rotation_table + block_idx;

        dcci(head_slot, SINGLE_CACHE_LINE);
        l2_swimlane_head = (__gm__ L2SwimlaneActiveHead *)(uintptr_t)(*head_slot);
    }

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
            __gm__ EslDispatchPayload *exec_payload = payload_base + (task_id & 1u);

            dcci(exec_payload, ENTIRE_DATA_CACHE);

            if (exec_payload->not_ready) {
                while (1) {
                    if (read_dmb_high32() == task_id) {
                        if ((uint32_t)read_reg(REG_ID_DATA_MAIN_BASE) == AICORE_EXIT_SIGNAL) {
                            write_reg(REG_ID_COND, AICORE_EXITED_VALUE);
                            return;
                        }
                        break;
                    }
                    if ((uint32_t)read_reg(REG_ID_DATA_MAIN_BASE) == AICORE_EXIT_SIGNAL) {
                        write_reg(REG_ID_COND, AICORE_EXITED_VALUE);
                        return;
                    }
                    SPIN_WAIT_HINT();
                }
            }

            write_reg(REG_ID_COND, MAKE_ACK_VALUE(task_id));

            ESL_SWIMLANE_AICORE_TASK_BEGIN(start_time);
            execute_task(exec_payload);
            ESL_SWIMLANE_AICORE_TASK_RECORD(swim_local, l2_swimlane_head, exec_payload, task_id, start_time);

            last_reg_val = reg_val;
            write_reg(REG_ID_COND, MAKE_FIN_VALUE(task_id));
        }
    }

    dcci(my_hank, SINGLE_CACHE_LINE, CACHELINE_OUT);
}
