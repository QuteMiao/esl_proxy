#define _POSIX_C_SOURCE 199309L

/*
cc -g -std=c11 -Wall -Wextra -pedantic -O2 -D_POSIX_C_SOURCE=199309L -I esl_proxy/include \
  -o /tmp/scheduler_test esl_proxy/src/scheduler/scheduler.c -lpthread
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "scheduler/conf.h"
// #include "scheduler/painter.h"
#include "scheduler/dispatch.h"

/* Global variable definitions needed by dispatch.c */
atomic_int g_completed_cnt = 0;
atomic_bool g_is_done = false;
atomic_bool g_orch_is_done = false;
atomic_int g_task_id = 0;
struct task_desc g_basic_buf[RING_SIZE];
ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

int g_worker_log = 0;

void log_write(const char *file, int line, const char *fmt, ...) { (void)file; (void)line; (void)fmt; }
void main_log_write(int line, const char *fmt, ...) { (void)line; (void)fmt; }

// #include "qwen3_dynamic_manual_scope.h"
void *worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    printf("%d,\n", tid);
    return NULL;
}

static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(void) {
    pthread_t dispatch_threads[DISPATCH_THREAD_CNT];
    pthread_t painter_threads[PAINTER_THREAD_CNT];

    // ring_buf_init();
    // init_predecessors();
    // init_ctrl_t();
    printf("painter,%d,dispatcher,%d\n", PAINTER_THREAD_CNT, DISPATCH_THREAD_CNT);
    uint64_t start_ns = get_time_ns();
    for (int i = 0; i < PAINTER_THREAD_CNT; i++) {
        pthread_create(&painter_threads[i], NULL, worker, (void *)(intptr_t)i);
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
    printf("%lld/ns\n", duration);
    return 0;
}
