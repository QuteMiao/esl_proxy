/* handshake.c — AICPU-AICore handshake state machine (algorithm layer, backend-neutral). */
#define _GNU_SOURCE

#include "handshake.h"

#include "log.h"
#include "memory_barrier.h"
#include "platform.h"
#include "platform_config.h"
#include "platform_regs.h"
#include "runtime.h"
#include "thread_yield.h"

#include <stddef.h>
#include <stdint.h>

/* per-core handshake spin bound */
#define HANDSHAKE_SPIN_MAX 100000000ULL
#define DEINIT_ACK_SPIN_MAX 50000000ULL
#define DEINIT_EXIT_REBROADCAST_INTERVAL 1000000ULL

static uint64_t g_core_reg_addrs[RUNTIME_MAX_WORKER];

int esl_handshake_start(EslRuntime *runtime)
{
    platform_handshake_aicore_bootstrap(runtime);
    if (get_platform_regs() == 0) {
        return 0;
    }
    return esl_handshake_all_cores(runtime);
}

static int wait_handshake_field(volatile uint32_t *field, uint32_t expect)
{
    uint64_t spins;

    for (spins = 0; spins < HANDSHAKE_SPIN_MAX; ++spins) {
        if (*field == expect) {
            return 1;
        }
    }
    return 0;
}

int esl_handshake_all_cores(EslRuntime *runtime)
{
    if (runtime == NULL) {
        return -1;
    }
    if (runtime->worker_count <= 0 || runtime->worker_count > RUNTIME_MAX_WORKER) {
        LOG_ERROR("Invalid worker_count %d", runtime->worker_count);
        return -1;
    }
    uint64_t *regs = (uint64_t *)get_platform_regs();
    if (regs == 0) {
        LOG_ERROR("Platform regs table not set");
        return -1;
    }
    LOG_INFO_V0("esl_proxy handshake for %d workers", runtime->worker_count);
    for (int i = 0; i < runtime->worker_count; i++) {
        EslHandshake *wk = &runtime->workers[i];

        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicpu_ready = 1;
        cache_flush_range((const void *)wk, sizeof(EslHandshake));
    }
    for (int i = 0; i < runtime->worker_count; i++) {
        EslHandshake *wk = &runtime->workers[i];
        uint32_t phys;
        uint64_t reg_addr;

        if (!wait_handshake_field(&wk->aicore_regs_ready, 1)) {
            LOG_ERROR("Core %d aicore_regs_ready timeout (regs_ready=%u aicpu_ready=%u)",
                      i, wk->aicore_regs_ready, wk->aicpu_ready);
            return -1;
        }
        cache_invalidate_range((const void *)&wk->physical_core_id, sizeof(wk->physical_core_id));
        phys = wk->physical_core_id;
        if (phys >= ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
            LOG_ERROR(
                "Worker %d invalid physical_core_id=%u (HAL table size=%u)", i, phys,
                (unsigned)ESL_PROXY_PLATFORM_HAL_REG_SLOTS);
            return -1;
        }
        reg_addr = regs[phys];
        if (reg_addr == 0) {
            LOG_ERROR("Worker %d physical_core_id=%u has zero register address", i, phys);
            return -1;
        }
        g_core_reg_addrs[i] = reg_addr;
        platform_init_aicore_regs(reg_addr);
        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicpu_regs_ready = 1;
        cache_flush_range((const void *)&wk->aicpu_regs_ready, sizeof(wk->aicpu_regs_ready));
        if (!wait_handshake_field((volatile uint32_t *)&wk->aicore_done, (uint32_t)(i + 1))) {
            LOG_ERROR("Core %d aicore_done timeout", i);
            return -1;
        }
    }
    return 0;
}

uint64_t esl_handshake_reg_addr(int core_idx)
{
    if (core_idx < 0 || core_idx >= RUNTIME_MAX_WORKER) {
        return 0;
    }
    return g_core_reg_addrs[core_idx];
}

static uint64_t shutdown_reg_addr(int worker_idx, uint64_t *regs)
{
    uint64_t reg_addr = g_core_reg_addrs[worker_idx];

    if (reg_addr == 0 && regs != NULL) {
        int hal_idx = esl_worker_to_hal_reg_index(worker_idx);

        if (hal_idx >= 0) {
            reg_addr = regs[hal_idx];
        }
    }
    return reg_addr;
}

void esl_shutdown_all_cores(EslRuntime *runtime)
{
    if (runtime == NULL) {
        return;
    }
    if (runtime->worker_count <= 0 || runtime->worker_count > RUNTIME_MAX_WORKER) {
        LOG_ERROR("Invalid worker_count %d", runtime->worker_count);
        return;
    }
    LOG_INFO_V0("esl_proxy shutting down %d AICore workers", runtime->worker_count);
    uint64_t *regs = (uint64_t *)get_platform_regs();
    if (regs == 0) {
        LOG_ERROR("Platform regs table not set");
        return;
    }

    /* Phase 1 — broadcast EXIT doorbell to every core (single writer pass). */
    for (int i = 0; i < runtime->worker_count; i++) {
        uint64_t reg_addr = shutdown_reg_addr(i, regs);

        if (reg_addr != 0) {
            write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICORE_EXIT_SIGNAL);
        }
    }
    ESL_WMB();

    /* Phase 2 — wait for each worker to post AICORE_EXITED_VALUE on COND, then
     * reset the slot to clean IDLE.  Never write AICPU_IDLE_TASK_ID while ack is
     * missing: that erases EXIT and leaves a sim worker spinning forever in
     * aicore_execute() (data_main_base == AICPU_IDLE_TASK_ID branch). */
    for (int i = 0; i < runtime->worker_count; i++) {
        uint64_t reg_addr = shutdown_reg_addr(i, regs);
        uint64_t spins;
        int acked;

        if (reg_addr == 0) {
            continue;
        }
        acked = 0;
        for (spins = 0; spins <= DEINIT_ACK_SPIN_MAX; ++spins) {
            if (platform_reg_worker_exit_acked(reg_addr)) {
                acked = 1;
                break;
            }
            if (spins > 0 && (spins % DEINIT_EXIT_REBROADCAST_INTERVAL) == 0ULL) {
                write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICORE_EXIT_SIGNAL);
                ESL_WMB();
            }
            ESL_PLATFORM_SCHED_YIELD();
        }
        if (!acked) {
            LOG_ERROR("Core %d deinit ack timeout (worker still running; EXIT retained)", i);
            continue;
        }

        write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICPU_IDLE_TASK_ID);
        write_reg(reg_addr, REG_ID_FAST_PATH_ENABLE, REG_SPR_FAST_PATH_CLOSE);
    }
}
