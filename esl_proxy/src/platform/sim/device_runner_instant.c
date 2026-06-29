/*
 * device_runner_instant.c — single manager thread for all sim AICore slots.
 * Handshake + doorbell poll; tasks complete instantly (no fake_kernel_run).
 */
#include "device_runner_instant.h"

#include "memory_barrier.h"
#include "platform_config.h"
#include "platform_regs.h"
#include "runtime.h"
#include "thread_yield.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    EslRuntime *runtime;
    uint64_t regs_table;
} sim_instant_manager_arg_t;

#define INSTANT_HANDSHAKE_SPIN_MAX 100000000ULL

static int instant_wait_u32(volatile uint32_t *field, uint32_t expect)
{
    uint64_t spins;

    for (spins = 0; spins < INSTANT_HANDSHAKE_SPIN_MAX; ++spins) {
        if (*field == expect) {
            return 0;
        }
        ESL_PLATFORM_SCHED_YIELD();
    }
    return -1;
}

static int instant_run_handshake(EslRuntime *runtime, uint64_t *regs)
{
    int n = runtime->worker_count;
    int i;

    for (i = 0; i < n; ++i) {
        EslHandshake *wk = &runtime->workers[i];
        int hal_idx;
        uint64_t reg_addr;

        if (instant_wait_u32(&wk->aicpu_ready, 1) != 0) {
            return -1;
        }

        hal_idx = esl_worker_to_hal_reg_index(i);
        if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
            return -1;
        }

        wk->physical_core_id = (uint32_t)hal_idx;
        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicore_regs_ready = 1;

        if (instant_wait_u32(&wk->aicpu_regs_ready, 1) != 0) {
            return -1;
        }

        reg_addr = regs[hal_idx];
        if (reg_addr == 0) {
            return -1;
        }

        write_reg(reg_addr, REG_ID_COND, AICORE_IDLE_VALUE);
        wk->core_type = runtime->workers[i].core_type;
        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicore_done = (uint32_t)(i + 1);
    }

    return 0;
}

static void instant_poll_cores(EslRuntime *runtime, uint64_t *regs, uint32_t *last_task_id,
                               uint8_t *core_exited)
{
    int n = runtime->worker_count;
    int i;

    for (i = 0; i < n; ++i) {
        int hal_idx;
        uint64_t reg_addr;
        uint32_t reg_val;

        if (core_exited[i]) {
            continue;
        }

        hal_idx = esl_worker_to_hal_reg_index(i);
        if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
            continue;
        }
        reg_addr = regs[hal_idx];
        if (reg_addr == 0) {
            continue;
        }

        reg_val = (uint32_t)read_reg(reg_addr, REG_ID_DATA_MAIN_BASE);
        if (reg_val == (uint32_t)AICORE_EXIT_SIGNAL) {
            write_reg(reg_addr, REG_ID_COND, AICORE_EXITED_VALUE);
            core_exited[i] = 1;
            continue;
        }

        if (reg_val == (uint32_t)AICPU_IDLE_TASK_ID || reg_val == last_task_id[i]) {
            continue;
        }

        write_reg(reg_addr, REG_ID_COND, MAKE_ACK_VALUE(reg_val));
        write_reg(reg_addr, REG_ID_COND, MAKE_FIN_VALUE(reg_val));
        last_task_id[i] = reg_val;
    }
}

static int instant_all_cores_exited(const uint8_t *core_exited, int n)
{
    int i;

    for (i = 0; i < n; ++i) {
        if (!core_exited[i]) {
            return 0;
        }
    }
    return 1;
}

void *sim_instant_aicore_manager_main(void *arg)
{
    sim_instant_manager_arg_t *ctx = (sim_instant_manager_arg_t *)arg;
    EslRuntime *runtime = ctx->runtime;
    uint64_t *regs = (uint64_t *)ctx->regs_table;
    uint32_t last_task_id[RUNTIME_MAX_WORKER];
    uint8_t core_exited[RUNTIME_MAX_WORKER];
    int n;
    int i;

    if (runtime == NULL || regs == NULL) {
        return NULL;
    }

    n = runtime->worker_count;
    if (n <= 0 || n > RUNTIME_MAX_WORKER) {
        return NULL;
    }

    for (i = 0; i < n; ++i) {
        last_task_id[i] = (uint32_t)AICPU_IDLE_TASK_ID;
        core_exited[i] = 0;
    }

    if (instant_run_handshake(runtime, regs) != 0) {
        return NULL;
    }

    while (!instant_all_cores_exited(core_exited, n)) {
        instant_poll_cores(runtime, regs, last_task_id, core_exited);
        ESL_PLATFORM_SCHED_YIELD();
    }

    return NULL;
}
