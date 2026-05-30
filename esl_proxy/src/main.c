#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

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

    aicpu_orchestration_entry(0);
    return 0;
}
