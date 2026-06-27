/* handshake.c — AICPU-AICore handshake state machine (algorithm layer, backend-neutral). */
#define _GNU_SOURCE

#include "handshake.h"

#include "memory_barrier.h"
#include "platform.h"
#include "platform_config.h"
#include "platform_regs.h"
#include "runtime.h"

/* Logging: onboard routes to CANN DLOG; sim has no CANN, so print to stdio.
 * (MAIN_LOGF's arg counter caps at 5 substitutions; these logs need more.) */
#if defined(ESL_PROXY_ONBOARD) || defined(ESL_PROXY_ONBOARD_HOST)
#include "onboard_log.h"
#else
#include <stdio.h>
#define LOG_ERROR(...)                          \
    do {                                        \
        (void)fprintf(stderr, "[handshake] ");  \
        (void)fprintf(stderr, __VA_ARGS__);     \
        (void)fprintf(stderr, "\n");            \
    } while (0)
#define LOG_INFO_V0(...)                        \
    do {                                        \
        (void)fprintf(stdout, "[handshake] ");  \
        (void)fprintf(stdout, __VA_ARGS__);     \
        (void)fprintf(stdout, "\n");            \
    } while (0)
#endif

#include <stddef.h>
#include <stdint.h>
#include <sched.h>

#define HANDSHAKE_SPIN_MAX 10000000ULL
#define DEINIT_ACK_SPIN_MAX 5000000ULL

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
        /* Periodically yield so peer threads (sim: AICore pthreads; onboard:
         * harmless on a real AICPU core) get scheduled and can publish the
         * field we are waiting on. Without this, a tight spin on a 320-host
         * can starve the 72 just-created AICore threads of CPU time during
         * the pthread startup window and time out the handshake. */
        if ((spins & 0x3FFU) == 0U) {
            sched_yield();
        }
    }
    return 0;
}

int esl_handshake_all_cores(EslRuntime *runtime)
{
    uint64_t *regs;
    uint64_t regs_table;
    int n;
    int n_handshake;
    int i;

    if (runtime == NULL) {
        return -1;
    }
    n = runtime->worker_count;
    if (n <= 0 || n > RUNTIME_MAX_WORKER) {
        LOG_ERROR("Invalid worker_count %d", n);
        return -1;
    }
    regs_table = get_platform_regs();
    if (regs_table == 0) {
        LOG_ERROR("Platform regs table not set");
        return -1;
    }
    regs = (uint64_t *)(uintptr_t)regs_table;
    n_handshake = n;
    LOG_INFO_V0("esl_proxy handshake for %d workers", n_handshake);
    for (i = 0; i < n_handshake; i++) {
        EslHandshake *wk = &runtime->workers[i];

        OUT_OF_ORDER_STORE_BARRIER();
        wk->aicpu_ready = 1;
        cache_flush_range((const void *)wk, sizeof(EslHandshake));
    }
    for (i = 0; i < n_handshake; i++) {
        EslHandshake *wk = &runtime->workers[i];
        uint32_t phys;
        uint64_t reg_addr;

        if (!wait_handshake_field(&wk->aicore_regs_ready, 1)) {
            int ready_cnt = 0;
            int first_not_ready = -1;
            int j;

            for (j = 0; j < n_handshake; j++) {
                EslHandshake *wj = &runtime->workers[j];

                cache_invalidate_range((const void *)wj, sizeof(EslHandshake));
                if (wj->aicore_regs_ready == 1) {
                    ready_cnt++;
                } else if (first_not_ready < 0) {
                    first_not_ready = j;
                }
            }
            cache_invalidate_range((const void *)wk, sizeof(EslHandshake));
            LOG_ERROR("Core %d aicore_regs_ready timeout: regs_ready=%u aicpu_ready=%u "
                      "ready_cnt=%d/%d first_not_ready=%d",
                      i, wk->aicore_regs_ready, wk->aicpu_ready, ready_cnt, n_handshake,
                      first_not_ready);
            return -1;
        }
        cache_invalidate_range((const void *)wk, sizeof(*wk));
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
        cache_flush_range((const void *)wk, sizeof(*wk));
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

void esl_shutdown_all_cores(EslRuntime *runtime)
{
    uint64_t *regs;
    uint64_t regs_table;
    int n;
    int n_shutdown;
    int i;

    if (runtime == NULL) {
        return;
    }
    n = runtime->worker_count;
    if (n <= 0 || n > RUNTIME_MAX_WORKER) {
        return;
    }
    n_shutdown = n;
    LOG_INFO_V0("esl_proxy shutting down %d AICore workers", n_shutdown);
    regs_table = get_platform_regs();
    regs = regs_table ? (uint64_t *)(uintptr_t)regs_table : NULL;
    for (i = 0; i < n_shutdown; i++) {
        uint64_t reg_addr = g_core_reg_addrs[i];

        if (reg_addr == 0 && regs != NULL) {
            int hal_idx = esl_worker_to_hal_reg_index(i);

            if (hal_idx >= 0) {
                reg_addr = regs[hal_idx];
            }
        }
        if (reg_addr != 0) {
            write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICORE_EXIT_SIGNAL);
        }
    }
    for (i = 0; i < n_shutdown; i++) {
        uint64_t reg_addr = g_core_reg_addrs[i];
        uint64_t spins;

        if (reg_addr == 0 && regs != NULL) {
            int hal_idx = esl_worker_to_hal_reg_index(i);

            if (hal_idx >= 0) {
                reg_addr = regs[hal_idx];
            }
        }
        if (reg_addr == 0) {
            continue;
        }
        spins = 0;
        while (read_reg(reg_addr, REG_ID_COND) != AICORE_EXITED_VALUE) {
            if (++spins > DEINIT_ACK_SPIN_MAX) {
                LOG_ERROR("Core %d deinit ack timeout (best-effort, continuing)", i);
                break;
            }
        }
    }
}
