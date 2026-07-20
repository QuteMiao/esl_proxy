/* platform_init.c — onboard platform bring-up (memory pool, handshake). */
#define _GNU_SOURCE

#include "platform.h"

#include "aicpu_runtime.h"
#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "executor.h"
#include "handshake.h"
#include "mem_pool.h"
#include "onboard_config.h"
#include "ring_buf.h"
#include "runtime.h"
#include "task.h"

#include <stddef.h>
#include <stdint.h>

#define ONBOARD_POOL_BASE ((void *)0x40000000000ULL)
#define ONBOARD_POOL_SIZE (64ULL * 1024 * 1024 * 1024)
#define ONBOARD_WHEN2FREE_CAP 4096

static when2free_entry_t g_onboard_when2free[ONBOARD_WHEN2FREE_CAP];

void init_predecessors(void);

void platform_handshake_aicore_bootstrap(EslRuntime *runtime)
{
    (void)runtime;
}

int esl_platform_init(EslRuntime *runtime)
{
    ring_buf_init();
    init_state_buf();
    init_predecessors();
    init_ctrl_t();
    mem_pool_init(&g_mem_pool, ONBOARD_POOL_BASE, ONBOARD_POOL_SIZE);
    mem_pool_init_fifo(&g_mem_pool, g_onboard_when2free, ONBOARD_WHEN2FREE_CAP);
    executor_init();
    if (runtime != NULL) {
        runtime->worker_count = ESL_PROXY_ONBOARD_WORKER_COUNT;
    }
    g_runtime = runtime;
    esl_init_global_context(runtime);

    if (esl_handshake_start(runtime) != 0) {
        return -1;
    }
    return 0;
}

void esl_platform_shutdown(EslRuntime *runtime)
{
    if (runtime != NULL) {
        esl_shutdown_all_cores(runtime);
    }
}

void platform_stats_publish(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt,
                            uint64_t commit, uint64_t ready_cube, uint64_t ready_vec,
                            uint64_t min_uncomplete, uint64_t elapsed_ns)
{
    (void)elapsed_ns;
    esl_write_stats(task_cnt, subtask_cnt, completed_cnt, commit, ready_cube, ready_vec, min_uncomplete);
}
