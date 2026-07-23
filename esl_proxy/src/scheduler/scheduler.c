#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "scheduler/conf.h"
#include "scheduler/painter.h"
#include "scheduler/dispatch.h"
#include "common/log.h"

/* Global variable definitions needed by dispatch.c and painter.c */
atomic_bool g_is_done = false;
atomic_int g_min_uncomplete_task = 0;

void *worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    printf("%d,\n", tid);
    return NULL;
}

int main(void) {
    pthread_t dispatch_threads[DISPATCH_THREAD_CNT];
    pthread_t painter_threads[PAINTER_THREAD_CNT];

    log_init("scheduler");
    g_worker_log = 1;

    buf_init();
    init_state_buf();
    init_ctrl_t();
    WORKER_LOGF("painter_cnt,%d,dispatcher_cnt,%d", PAINTER_THREAD_CNT, DISPATCH_THREAD_CNT);
    uint64_t start_ns = get_time_ns();
    for (int i = 0; i < PAINTER_THREAD_CNT; i++) {
        pthread_create(&painter_threads[i], NULL, painter, (void *)(intptr_t)i);
#ifdef __linux__
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(2*i, &mask);
        pthread_setaffinity_np(painter_threads[i], sizeof(cpu_set_t), &mask);
#endif
    }

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_create(&dispatch_threads[i], NULL, dispatch_worker, (void *)(intptr_t)i);
#ifdef __linux__
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET((2*i + 1), &mask);
        pthread_setaffinity_np(painter_threads[i], sizeof(cpu_set_t), &mask);
#endif
    }

    for (int i = 0; i < PAINTER_THREAD_CNT; i++) {
        pthread_join(painter_threads[i], NULL);
    }

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_join(dispatch_threads[i], NULL);
    }
    uint64_t end_ns = get_time_ns();
    uint64_t duration = end_ns - start_ns;
    WORKER_LOGF("scheduler_duration,%lld/ns", duration);
    log_close();
    return 0;
}
