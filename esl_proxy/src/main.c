#define _POSIX_C_SOURCE 199309L

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#if ORCHESTRATION_TIME
#include <time.h>
#endif

#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "executor.h"
#include "log.h"
#include "manager.h"
#include "mem_pool.h"

/* Orchestration case header. Override at build time, e.g.
 *   make CASE=qwen3_dynamic_tensormap.h
 * Defaults to qwen3 dynamic manual-scope (all-SPMD tier). */
#ifndef ORCH_CASE
#define ORCH_CASE qwen3_dynamic_manual_scope.h
#endif

/* Macro to stringify the include directive properly */
#define INCLUDE(x) #x
#define INCLUDE_FILE(x) INCLUDE(x)
#include INCLUDE_FILE(ORCH_CASE)

/* Forward declaration for the orchestration entry point provided by the case */
void aicpu_orchestration_entry(const uint64_t orch_args);

#define MEM_POOL_BYTES (1024UL * 1024UL * 1024UL)
#define WHEN2FREE_CAP 4096

static uint8_t g_mem_pool_storage[MEM_POOL_BYTES];
static when2free_entry_t g_when2free_entries[WHEN2FREE_CAP];

extern atomic_bool g_orch_is_done;

int main(void) {
    pthread_t dispatch_threads[DISPATCH_THREAD_CNT];
    pthread_t cutter_threads[CUTTER_THREAD_CNT];
    pthread_t executor_threads[EXECUTOR_THREAD_CNT];
#if ORCHESTRATION_TIME
    uint64_t total_start_ns = get_time_ns();
#endif

#if WORKER_LOG
    const char *log_env = getenv("WORKER_LOG");
    if (log_env != NULL && log_env[0] == '1') {
        g_worker_log = 1;
        log_init("pto.");
    }
#endif

    mem_pool_init(&g_mem_pool, g_mem_pool_storage, sizeof g_mem_pool_storage);
    mem_pool_init_fifo(&g_mem_pool, g_when2free_entries, WHEN2FREE_CAP);
    ring_buf_init();
    init_predecessors();
    init_ctrl_t();
    
    // executor_init();

    // pthread_create(&manager_thread, NULL, manager_worker, &g_mem_pool);

    // for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
    //     pthread_create(&dispatch_threads[i], NULL, dispatch_worker,
    //                    (void *)(intptr_t)i);
    // }

    for (int i = 0; i < CUTTER_THREAD_CNT; i++) {
        pthread_create(&cutter_threads[i], NULL, cutter_worker,
                       (void *)(intptr_t)i);
    }
    // for (int i = 0; i < EXECUTOR_THREAD_CNT; i++) {
    //     pthread_create(&executor_threads[i], NULL, executor_worker, (void *)(intptr_t)i);
    // }

#if ORCHESTRATION_TIME
    uint64_t start_ns = get_time_ns();
    aicpu_orchestration_entry(0);
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    MAIN_LOGF("[orchestration] task_cnt = %u", g_task_id);
    MAIN_LOGF("[orchestration] subtask_cnt = %llu", (unsigned long long)g_subtask_cnt);
    MAIN_LOGF("[orchestration] elapsed_time = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[orchestration] task_tp = %f MTasks/s", (float)(g_task_id * 1000.0 / elapsed_ns));
    MAIN_LOGF("[orchestration] subtask_tp = %f MTasks/s", (float)(g_subtask_cnt * 1000.0 / elapsed_ns));
#else
    aicpu_orchestration_entry(0);
#endif
    atomic_store(&g_orch_is_done, true);
    // for (int i = 0; i < EXECUTOR_THREAD_CNT; i++) {
    //     pthread_join(executor_threads[i], NULL);
    // }
    for (int i = 0; i < CUTTER_THREAD_CNT; i++) {
        pthread_join(cutter_threads[i], NULL);
    }
    // for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
    //     pthread_join(dispatch_threads[i], NULL);
    // }
    // pthread_join(manager_thread, NULL);

#if WORKER_LOG
    log_close();
#endif

    return 0;
}