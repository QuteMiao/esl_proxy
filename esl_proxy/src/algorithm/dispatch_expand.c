/*
 * dispatch_expand.c — onboard/PTO2 dispatch extensions split out of dispatch.c.
 *
 * Holds the functions added on top of the base scheduler loop: PTO2 payload
 * build/publish, per-core register mapping, hardware submit/poll, completion
 * drain, and final-stats publish. The base loop (set_mix/get_free_exe/
 * push_2_completed_queue/send_task/dispatch/dispatch_worker) stays in dispatch.c.
 */
#define _GNU_SOURCE

#include "dispatch.h"
#include "handshake.h"
#include "runtime.h"

#include "cutter.h"
#include "executor.h"
#include "log.h"
#include "memory_barrier.h"
#include "ring_buf.h"
#include "spin.h"
#include "swimlane_aicpu.h"

#include "platform.h"
#include "platform_config.h"
#include "platform_regs.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <stdatomic.h>

#define ESL_FAKE_KERNEL_FUNC_ID_AIC 0U
#define ESL_FAKE_KERNEL_FUNC_ID_AIV 1U

extern struct task_desc g_basic_buf[RING_SIZE];

extern atomic_int g_task_id;
extern atomic_int g_completed_cnt;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern PredecessorCnt g_predecessor_cnt[RING_SIZE];
extern int g_subtask_cnt;
extern executor_t g_executors[EXE_TYPE_CNT][AIC_CNT];

static uint32_t g_core_dispatch_seq[RUNTIME_MAX_WORKER];
EslRuntime *g_runtime;


static uint32_t dispatch_next_reg_task_id(int phys)
{
    uint32_t seq;
    uint32_t reg_id;

    if (phys < 0 || phys >= RUNTIME_MAX_WORKER) {
        return 0;
    }
    seq = ++g_core_dispatch_seq[phys];
    reg_id = seq & (uint32_t)TASK_ID_MASK;
    if (reg_id >= (uint32_t)AICORE_EXIT_SIGNAL) {
        g_core_dispatch_seq[phys] = seq + ((uint32_t)AICORE_EXIT_SIGNAL - reg_id);
        reg_id = (uint32_t)(g_core_dispatch_seq[phys] & (uint32_t)TASK_ID_MASK);
    }
    return reg_id;
}

static void build_payload(EslDispatchPayload *out, const struct task_desc *desc, uint32_t block_idx,
                          uint64_t fake_kernel_addr)
{
    if (out == NULL || desc == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->function_bin_addr = fake_kernel_addr;
    out->args[0] = (uint64_t)desc->duration;
    out->args[1] = (uint64_t)desc->jitter_mask;
    out->local_block_idx = (int32_t)block_idx;
    out->local_block_num = (int32_t)desc->count;
    out->async_task_token = UINT64_MAX;
    out->args[48] = (uint64_t)(uintptr_t)&out->local_block_idx;
    out->args[49] = (uint64_t)(uintptr_t)&out->global_sub_block_id;
    out->not_ready = 0U;
}

void esl_init_global_context(EslRuntime *runtime)
{
    int i;
    int slot;

    if (runtime == NULL) {
        return;
    }

    for (i = 0; i < runtime->worker_count && i < RUNTIME_MAX_WORKER; ++i) {
        uint64_t base = runtime->workers[i].task;
        if (base == 0) {
            continue;
        }
        for (slot = 0; slot < 2; ++slot) {
            EslDispatchPayload *p =
                (EslDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslDispatchPayload));
            if (runtime->workers[i].core_type != 0) {
                int aiv_idx = i - ESL_PROXY_ONBOARD_BLOCK_DIM;
                p->global_sub_block_id = (aiv_idx >= 0) ? (aiv_idx % ESL_PROXY_AIV_LANES_PER_BLOCK) : 0;
            } else {
                p->global_sub_block_id = 0;
            }
        }
    }
}

EslPublishHandle esl_prepare_subtask_to_core(EslRuntime *runtime, int core, uint16_t task_id, uint32_t block_idx)
{
    EslPublishHandle handle = {0, 0};
    EslDispatchPayload *p;
    uint64_t base;
    uint32_t reg_task_id;
    int slot;
    const struct task_desc *desc;
    uint64_t fake_kernel_addr;

    if (runtime == NULL || core < 0 || core >= RUNTIME_MAX_WORKER) {
        return handle;
    }

    base = runtime->workers[core].task;
    if (base == 0) {
        return handle;
    }

    reg_task_id = dispatch_next_reg_task_id(core);
    slot = (int)(reg_task_id & 1u);
    p = (EslDispatchPayload *)(uintptr_t)(base + (uint64_t)slot * sizeof(EslDispatchPayload));
    desc = &g_basic_buf[task_id & RING_MASK];
    fake_kernel_addr = runtime->func_id_to_addr_[runtime->workers[core].core_type == 0
                                                      ? ESL_FAKE_KERNEL_FUNC_ID_AIC
                                                      : ESL_FAKE_KERNEL_FUNC_ID_AIV];
    build_payload(p, desc, block_idx, fake_kernel_addr);
    handle.reg_task_id = reg_task_id;
    return handle;
}

void esl_publish_subtask_to_core(EslPublishHandle handle)
{
    if (handle.reg_addr == 0U || handle.reg_task_id == 0U) {
        return;
    }
    write_reg(handle.reg_addr, REG_ID_DATA_MAIN_BASE, handle.reg_task_id);
}

uint64_t dispatch_core_reg_addr(int worker_id)
{
    uint64_t reg_addr = esl_handshake_reg_addr(worker_id);

    if (reg_addr != 0) {
        return reg_addr;
    }
    const uint64_t table = get_platform_regs();
    int hal_idx;

    if (table == 0) {
        return 0;
    }
    hal_idx = esl_worker_to_hal_reg_index(worker_id);
    if (hal_idx < 0 || hal_idx >= (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS) {
        return 0;
    }
    return ((uint64_t *)table)[hal_idx];
}

void dispatch_mark_slot_complete(int exe_type, int core, int slot, uint64_t reg_addr,
                                 uint32_t reg_task)
{
    const uint64_t mask = (uint64_t)1 << core;

    if (!platform_reg_task_finished(reg_addr, reg_task)) {
        return;
    }
    platform_reg_task_ack(reg_addr, reg_task);
    g_ctrl_t[0].msg_bitmap[exe_type][slot] |= mask;
    g_executors[exe_type][core].idx = (uint8_t)AIC_OSTD;
    g_executors[exe_type][core].tasks[slot] = EXEC_SLOT_EMPTY;
}

/* 把 AICore 完成事件拉到 msg_bitmap，供 push_2_completed_queue/get_completed 解码。 */
void dispatch_poll(int tid)
{
    (void)tid;
    if (g_runtime == NULL) {
        return;
    }
    const int n_workers = g_runtime->worker_count;
    const int n_cores = AIC_CNT;

    for (int exe_type = 0; exe_type < EXE_TYPE_CNT; exe_type++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            for (int core = 0; core < n_cores; core++) {
                uint16_t task_id = g_executors[exe_type][core].tasks[slot];

                if (task_id == EXEC_SLOT_EMPTY) {
                    continue;
                }
                uint64_t mask = (uint64_t)1 << core;
                if (g_ctrl_t[0].msg_bitmap[exe_type][slot] & mask) {
                    continue;
                }
                const int phys = (int)g_executors[exe_type][core].block_idx[slot];
                if (phys < 0 || phys >= n_workers) {
                    continue;
                }
                const uint32_t reg_task = (uint32_t)g_executors[exe_type][core].base[slot];
                if (reg_task == 0U) {
                    continue;
                }
                const uint64_t reg_addr = dispatch_core_reg_addr(phys);
                if (reg_addr != 0 && platform_reg_task_finished(reg_addr, reg_task)) {
                    dispatch_mark_slot_complete(exe_type, core, slot, reg_addr, reg_task);
                }
            }
        }
    }
}

void dispatch_publish_final_stats(uint64_t elapsed_ns)
{
    int end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    int first_uncomp = -1;
    int n_uncomp = 0;

    for (int i = 0; i < end; i++) {
        if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
            if (first_uncomp < 0) {
                first_uncomp = i;
            }
            n_uncomp++;
        }
    }

    uint64_t pred0 = (first_uncomp >= 0) ? (uint64_t)g_predecessor_cnt[first_uncomp].v : 0;
    uint64_t rqc = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_CUBE].cnt;
    uint64_t rqv = (uint64_t)g_ctrl_t[0].ready_queue[TASK_TYPE_VECTOR].cnt;

    platform_stats_publish((uint64_t)end, (uint64_t)g_subtask_cnt, (uint64_t)g_completed_cnt,
                           ((uint64_t)(uint32_t)atomic_load_explicit(&g_commit_task_id, memory_order_acquire)),
                           (uint64_t)n_uncomp, ((uint64_t)(uint32_t)first_uncomp) | (pred0 << 32),
                           (rqc & 0xffffffffULL) | (rqv << 32), elapsed_ns);
}
