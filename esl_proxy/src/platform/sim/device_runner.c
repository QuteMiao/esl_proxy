/*
 * device_runner.c — sim AICore worker thread lifecycle (mirror onboard AICore launch).
 */
#include "device_runner.h"

#include "platform.h"
#include "platform_config.h"
#include "platform_regs.h"
#include "runtime.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#if ESL_PROXY_SIM_INSTANT_AICORE
#include "device_runner_instant.h"

typedef struct {
    EslRuntime *runtime;
    uint64_t regs_table;
} sim_instant_manager_arg_t;

static pthread_t g_manager_thread;
static sim_instant_manager_arg_t g_manager_arg;
static int g_manager_started;

int esl_sim_aicore_workers_start(EslRuntime *runtime)
{
    uint64_t regs_table;

    if (runtime == NULL) {
        return -1;
    }
    if (runtime->worker_count <= 0 || runtime->worker_count > PLATFORM_HOST_WORKER_COUNT) {
        return -1;
    }
    regs_table = get_platform_regs();
    if (regs_table == 0) {
        return -1;
    }

    g_manager_arg.runtime = runtime;
    g_manager_arg.regs_table = regs_table;
    if (pthread_create(&g_manager_thread, NULL, sim_instant_aicore_manager_main, &g_manager_arg) != 0) {
        return -1;
    }
    g_manager_started = 1;
    return 0;
}

void esl_sim_aicore_workers_stop(void)
{
    if (g_manager_started) {
        pthread_join(g_manager_thread, NULL);
        g_manager_started = 0;
    }
}

#else /* !ESL_PROXY_SIM_INSTANT_AICORE */

typedef struct {
    EslRuntime *runtime;
    int block_idx;
    int core_type;
    uint64_t regs_table;
} sim_aicore_thread_arg_t;

extern void aicore_execute_wrapper(EslRuntime *runtime, int block_idx, int core_type,
                                   uint32_t physical_core_id, uint64_t regs_table,
                                   uint32_t profiling_flag, uint64_t rotation_table);

static pthread_t g_aicore_threads[PLATFORM_HOST_WORKER_COUNT];
static sim_aicore_thread_arg_t g_aicore_args[PLATFORM_HOST_WORKER_COUNT];
static int g_aicore_worker_count;

static void *sim_aicore_thread_main(void *arg)
{
    sim_aicore_thread_arg_t *ctx = (sim_aicore_thread_arg_t *)arg;
    int hal_idx;

    hal_idx = esl_worker_to_hal_reg_index(ctx->block_idx);
    aicore_execute_wrapper(ctx->runtime, ctx->block_idx, ctx->core_type, (uint32_t)hal_idx,
                           ctx->regs_table, 0U, 0U);
    return NULL;
}

int esl_sim_aicore_workers_start(EslRuntime *runtime)
{
    int i;
    int n;
    uint64_t regs_table;

    if (runtime == NULL) {
        return -1;
    }
    n = runtime->worker_count;
    if (n <= 0 || n > PLATFORM_HOST_WORKER_COUNT) {
        return -1;
    }
    regs_table = get_platform_regs();
    if (regs_table == 0) {
        return -1;
    }

    g_aicore_worker_count = n;
    for (i = 0; i < n; ++i) {
        g_aicore_args[i].runtime = runtime;
        g_aicore_args[i].block_idx = i;
        g_aicore_args[i].core_type = runtime->workers[i].core_type;
        g_aicore_args[i].regs_table = regs_table;
        if (pthread_create(&g_aicore_threads[i], NULL, sim_aicore_thread_main, &g_aicore_args[i]) != 0) {
            g_aicore_worker_count = i;
            esl_sim_aicore_workers_stop();
            return -1;
        }
    }
    return 0;
}

void esl_sim_aicore_workers_stop(void)
{
    int i;

    for (i = 0; i < g_aicore_worker_count; ++i) {
        pthread_join(g_aicore_threads[i], NULL);
    }
    g_aicore_worker_count = 0;
}

#endif /* ESL_PROXY_SIM_INSTANT_AICORE */
