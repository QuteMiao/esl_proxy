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
#include "log.h"
#include "manager.h"
#include "mem_pool.h"
#include "qwen3_decode.h"

#define MEM_POOL_BYTES (512UL * 1024UL * 1024UL)
#define WHEN2FREE_CAP 4096

static uint8_t g_mem_pool_storage[MEM_POOL_BYTES];
static when2free_entry_t g_when2free_entries[WHEN2FREE_CAP];

#if ORCHESTRATION_TIME
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

int main(void) {
    pthread_t dispatch_threads[DISPATCH_THREAD_CNT];
    pthread_t cutter_threads[CUTTER_THREAD_CNT];
    pthread_t manager_thread;

#if WORKER_LOG
    const char *log_env = getenv("WORKER_LOG");
    if (log_env != NULL && log_env[0] == '1')
        g_worker_log = 1;
#endif

    mem_pool_init(&g_mem_pool, g_mem_pool_storage, sizeof g_mem_pool_storage);
    mem_pool_init_fifo(&g_mem_pool, g_when2free_entries, WHEN2FREE_CAP);
    ring_buf_init();

    pthread_create(&manager_thread, NULL, manager_worker, &g_mem_pool);

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_create(&dispatch_threads[i], NULL, dispatch_worker,
                       (void *)(intptr_t)i);
    }

    for (int i = 0; i < CUTTER_THREAD_CNT; i++) {
        pthread_create(&cutter_threads[i], NULL, cutter_worker,
                       (void *)(intptr_t)i);
    }

#if ORCHESTRATION_TIME
    uint64_t start_ns = get_time_ns();
    aicpu_orchestration_entry(0);
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    /* Subtask count: an SPMD task expands into block_num (count) subtasks; every
     * non-SPMD task is a single subtask. Computed after orchestration so it does
     * not count toward elapsed_time. */
    uint64_t subtask_cnt = 0;
    for (uint32_t i = 1; i <= g_task_id; i++) {
        const struct task_desc *t = &g_basic_buf[i & RING_MASK];
        if (t->mode == ORG_MODE_SPMD_SYNC || t->mode == ORG_MODE_SPMD_ASYNC)
            subtask_cnt += t->count;
        else
            subtask_cnt += 1;
    }

    fprintf(stderr, "[orchestration] task_cnt=%d\n", g_task_id);
    fprintf(stderr, "[orchestration] subtask_cnt=%llu\n",
            (unsigned long long)subtask_cnt);
    fprintf(stderr, "[orchestration] elapsed_time=%llu ns\n",
            (unsigned long long)elapsed_ns);
    fprintf(stderr, "[orchestration] time_240_task=%llu ns\n",
            (unsigned long long)(elapsed_ns / g_task_id * 240));
    fprintf(stderr, "[orchestration] time_240_subtask=%llu ns\n",
            (unsigned long long)(elapsed_ns / subtask_cnt * 240));
#else
    aicpu_orchestration_entry(0);
#endif
    return 0;
}