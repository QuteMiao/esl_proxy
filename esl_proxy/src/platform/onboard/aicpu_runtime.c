/* aicpu_runtime.c — onboard AICPU orchestration (CANN entry + cutter/dispatch/orch threads). */
#define _GNU_SOURCE

#include "aicpu_runtime.h"
#include "aicpu_affinity.h"
#include "conf.h"
#include "cutter.h"
#include "dispatch.h"
#include "dlog_pub.h"
#include "kernel_args.h"
#include "memory_barrier.h"
#include "platform.h"
#include "onboard_config.h"
#include "onboard_log.h"
#include "platform_regs.h"
#include "ring_buf.h"
#include "spin.h"
#include "swimlane_aicpu.h"
#include "task.h"
#include "tools.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ORCH_CASE
#define ORCH_CASE paged_attention_unroll_manual_scope.h
#endif
#define INCLUDE(x) #x
#define INCLUDE_FILE(x) INCLUDE(x)

void aicpu_orchestration_entry(uint64_t orch_args);

extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern atomic_int g_task_id;
extern task_state g_state_buf[RING_SIZE];
extern uint16_t g_predecessor_cnt[RING_SIZE];
extern atomic_int g_completed_cnt;
extern atomic_bool g_orch_is_done;
extern atomic_bool g_is_done;
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

static atomic_int g_thread_idx;
static atomic_int g_finished_count;
static atomic_int g_workers_ready;
static atomic_bool g_init_done;
static atomic_bool g_init_failed;
static atomic_flag g_once = ATOMIC_FLAG_INIT;
static uint64_t g_device_start_cycle;
volatile uint64_t *g_esl_stats_base;

static int init_once(EslRuntime *runtime, int thread) {
    (void)thread;
    if (atomic_flag_test_and_set_explicit(&g_once, memory_order_acquire)) {
        while (!atomic_load_explicit(&g_init_done, memory_order_acquire) &&
               !atomic_load_explicit(&g_init_failed, memory_order_acquire)) {
        }
        return atomic_load_explicit(&g_init_failed, memory_order_acquire) ? -1 : 0;
    }
    if (esl_platform_init(runtime) != 0) {
        LOG_ERROR("init_once: esl_platform_init failed");
        atomic_store_explicit(&g_init_failed, true, memory_order_release);
        return -1;
    }
    atomic_store_explicit(&g_init_done, true, memory_order_release);
    return 0;
}

int32_t esl_aicpu_execute(EslRuntime *runtime) {
    int idx;
    int finished;

    if (runtime == NULL) {
        LOG_ERROR("esl_aicpu_execute: null runtime");
        return -1;
    }
    if (init_once(runtime, -1) != 0) {
        LOG_ERROR("esl_aicpu_execute: init_once failed");
        return -1;
    }
    cache_invalidate_range(runtime, sizeof(EslRuntime));
    /* CPU 亲和性门控(对齐 simpler):host 探测到允许的控制 CPU 时,从 launch_count 个
     * 被拉起的线程里钉出 allowed_count 个并赋予角色(0..allowed_count-1,最后=orch),其余
     * 线程作为多余线程干净退出。host 探测失败时回退老行为:按 g_thread_idx 顺序分配角色。 */
    if (runtime->aicpu_allowed_cpu_count > 0) {
        int exec_idx = -1;
        if (!esl_aicpu_affinity_gate(runtime->aicpu_allowed_cpus, runtime->aicpu_allowed_cpu_count,
                                     runtime->aicpu_launch_count, &exec_idx)) {
            return 0; /* 多余线程 — 干净退出 */
        }
        idx = exec_idx; /* 角色来自门控(0..allowed_count-1;最后=orch) */
    } else {
        idx = atomic_fetch_add_explicit(&g_thread_idx, 1, memory_order_acq_rel); /* 回退:老行为 */
    }
    LOG_ERROR("AICPU thread %d enter execute", idx);

    if (idx < ESL_PROXY_AICPU_THREAD_NUM) {
        if (atomic_fetch_add_explicit(&g_workers_ready, 1, memory_order_acq_rel) + 1 ==
            ESL_PROXY_AICPU_THREAD_NUM) {
            atomic_thread_fence(memory_order_release);
        } else {
            while (atomic_load_explicit(&g_workers_ready, memory_order_acquire) <
                   ESL_PROXY_AICPU_THREAD_NUM) {
                spin_wait();
            }
        }
    }

    switch (idx) {
    case ESL_AICPU_ROLE_CUTTER:
        cutter_worker((void *)0);
        break;
    case ESL_AICPU_ROLE_DISPATCH:
        dispatch_worker((void *)0);
        break;
    case ESL_AICPU_ROLE_ORCH:
        aicpu_orchestration_entry(0);
        atomic_store_explicit(&g_orch_is_done, true, memory_order_release);
        wmb();
        break;
    default:
        while (atomic_load_explicit(&g_finished_count, memory_order_acquire) <
               ESL_PROXY_AICPU_THREAD_NUM) {
            spin_wait();
        }
        return 0;
    }

    finished = atomic_fetch_add_explicit(&g_finished_count, 1, memory_order_acq_rel) + 1;
    if (finished == ESL_PROXY_AICPU_THREAD_NUM) {
        esl_platform_shutdown(runtime);
    }
    return 0;
}

void esl_write_stats(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt, uint64_t commit,
    uint64_t ready_cube, uint64_t ready_vec, uint64_t min_uncomplete) {
    if (g_esl_stats_base != NULL) {
        g_esl_stats_base[0] = task_cnt;
        g_esl_stats_base[1] = subtask_cnt;
        g_esl_stats_base[2] = completed_cnt;
        g_esl_stats_base[4] = commit;
        g_esl_stats_base[5] = ready_cube;
        g_esl_stats_base[6] = ready_vec;
        g_esl_stats_base[7] = min_uncomplete;
        cache_flush_range((const void *)g_esl_stats_base, 8 * sizeof(uint64_t));
    }
}

__attribute__((visibility("default"))) int esl_aicpu_init(void *arg) {
    KernelArgs *k_args;

    init_log_switch();
    if (arg == NULL) {
        LOG_ERROR("%s", "Invalid kernel arguments: null pointer");
        return -1;
    }
    k_args = (KernelArgs *)arg;
    set_log_level((int)k_args->log_level);
    set_log_info_v((int)k_args->log_info_v);
    g_device_start_cycle = esl_onboard_sys_cnt();
    if (k_args->device_wall_data_base != 0) {
        *(uint64_t *)(uintptr_t)k_args->device_wall_data_base = 0;
    }
    ESL_SWIMLANE_AICPU_SET_BASE(k_args->l2_swimlane_data_base);
    ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(k_args->l2_swimlane_aicore_rotation_table);
    ESL_SWIMLANE_AICPU_SET_ENABLED(ESL_SWIMLANE_IS_FLAG_ON(k_args->enable_profiling_flag));
    LOG_INFO_V0("%s", "esl_proxy AICPU Init");
    return 0;
}

__attribute__((visibility("default"))) int esl_aicpu_exec(void *arg) {
    KernelArgs *k_args;
    EslRuntime *runtime;
    int rc;
    uint64_t my_end;

    if (arg == NULL) {
        LOG_ERROR("%s", "Invalid kernel arguments: null pointer");
        return -1;
    }
    k_args = (KernelArgs *)arg;
    runtime = (EslRuntime *)(uintptr_t)k_args->runtime_args;
    if (runtime == NULL) {
        LOG_ERROR("%s", "Invalid runtime_args: null pointer");
        return -1;
    }
    ESL_SWIMLANE_AICPU_SET_BASE(k_args->l2_swimlane_data_base);
    ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(k_args->l2_swimlane_aicore_rotation_table);
    ESL_SWIMLANE_AICPU_SET_ENABLED(ESL_SWIMLANE_IS_FLAG_ON(k_args->enable_profiling_flag));
    set_log_level((int)k_args->log_level);
    set_log_info_v((int)k_args->log_info_v);
    set_platform_regs(k_args->regs);
    g_esl_stats_base = (volatile uint64_t *)(uintptr_t)k_args->device_wall_data_base;
    LOG_ERROR("esl_aicpu_exec: start");
    rc = esl_aicpu_execute(runtime);
    LOG_ERROR("esl_aicpu_exec: rc=%d", rc);
    if (rc != 0) {
        LOG_ERROR("esl_aicpu_execute failed rc=%d", rc);
        return rc;
    }
    my_end = esl_onboard_sys_cnt();
    if (g_esl_stats_base != NULL && my_end > g_device_start_cycle) {
        g_esl_stats_base[3] = (uint64_t)(cycles_to_us(my_end - g_device_start_cycle) * 1000.0);
        cache_flush_range((const void *)g_esl_stats_base, 4 * sizeof(uint64_t));
    }
    return rc;
}

#include INCLUDE_FILE(ORCH_CASE)
