/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 *
 * MIX (TASK_TYPE_MIX == 2) model for reviewers
 * --------------------------------------------
 * Hardware is modeled as two tracks (CUBE / VECTOR), same core index = one
 * implicit block.  free_bitmap[CUBE|VECTOR] are authoritative busy/free bits;
 * free_bitmap[MIX] is derived as (cube & vector) and only used to pick dual-
 * free indices.  Sending a MIX task occupies BOTH tracks at that index with
 * the same task_id.  Completing it enqueues the id once after BOTH sides
 * report (see collect_completed / mix_side_done).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scheduler/dispatch.h"
#include "common/task.h"
#include "common/log.h"
#include "platform/a6.h"

extern atomic_bool g_is_done;

ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

/* Refresh free_bitmap[MIX] from CUBE∩VECTOR. Never writes CUBE/VECTOR bits. */
static inline void set_mix(int tid)
{
    for (int j = 0; j < AIC_OSTD; j++) {
        g_ctrl_t[tid].free_bitmap[TASK_TYPE_MIX][j] =
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_CUBE][j] &
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_VECTOR][j];
    }
}

void init_ctrl_t(void)
{
    for (int tid = 0; tid < DISPATCH_THREAD_CNT; tid++) {
        g_ctrl_t[tid].tid = (uint32_t)tid;
        if (AIC_CNT_PER_THREAD >= 64) {
            g_ctrl_t[tid].aicore_mask = ~0ULL;
        } else {
            g_ctrl_t[tid].aicore_mask = ~0ULL >> (64 - AIC_CNT_PER_THREAD);
        }

        /* Init only CUBE/VECTOR free maps; MIX is derived via set_mix(). */
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].free_bitmap[i][j] = g_ctrl_t[tid].aicore_mask;
            }
        }
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].msg_bitmap[i][j] = 0x0;
            }
        }

        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_CNT; j++) {
                g_ctrl_t[tid].task_id_map1[i][j] = 0;
                g_ctrl_t[tid].task_id_map2[i][j] = 0;
            }
        }
        memset(g_ctrl_t[tid].mix_side_done, 0, sizeof(g_ctrl_t[tid].mix_side_done));
        set_mix(tid);

        uint64_t base = 0;
        uint64_t idx = 0;
        for (size_t i = 0; i < EXE_TYPE_CNT; i++)
        {
            idx = 0;
            for (size_t j = AIC_CNT_PER_THREAD * tid; j < AIC_CNT_PER_THREAD * (tid + 1); j++)
            {
                base = AICORE_SPR_BASE;
                base += (i == 0 ? AICORE_CUBE_OFFSET : AICORE_VECTOR_OFFSET);
                if (j >= AIC_CNT_PER_DIE) {
                    base += AICORE_DIE_OFFSET + AICORE_OFFSET * (j - AIC_CNT_PER_DIE);
                } else {
                    base += AICORE_OFFSET * j;
                }

                g_ctrl_t[tid].aicore_spr_1[i][idx] = (uint64_t*)base;
                g_ctrl_t[tid].aicore_spr_2[i][idx] = (uint64_t*)(base + AICORE_SPR_OFFSET);
                idx++;
            }
        }

        for (int i = 0; i < TASK_TYPE_CNT; i++) {
            memset(&g_ctrl_t[tid].ready_queue[i], 0, sizeof(queue_t));
            atomic_flag_clear_explicit(&g_ctrl_t[tid].ready_queue[i].lock, memory_order_release);
        }
        memset(&g_ctrl_t[tid].completed_queue, 0, sizeof(queue_t));
        atomic_flag_clear_explicit(&g_ctrl_t[tid].completed_queue.lock, memory_order_release);
        memset(&g_ctrl_t[tid].remote_completed_queue, 0, sizeof(queue_t));
        atomic_flag_clear_explicit(&g_ctrl_t[tid].remote_completed_queue.lock, memory_order_release);
    }
}

static void hand_shake(int cpu_idx, uint64_t* aicore_spr[], int type, int ostd2_offset) {
    uint64_t base = AICPU_MSGQ_BASE + cpu_idx * AICPU_OFFSET + ostd2_offset * AICPU_MSGQ_OFFSET;
    uint64_t msgq_addr = 0;

    for (size_t i = 0; i < AIC_CNT_PER_THREAD; i++)
    {
        uint64_t offset = type == 0 ? 0 : 128;
        msgq_addr = base + (i + offset)  * AICPU_MSGQ_OFFSET;
        #ifdef REAL_CHIP
        *aicore_spr[i] = HAND_SHAKE_VAL | (msgq_addr & LOAW_ADDR_MASK);
        #endif
        (void)msgq_addr;
    }
}

static inline void read_msgq(int tid)
{
    #ifdef REAL_CHIP
    READ_REG(g_ctrl_t[tid].msg_bitmap[0][0], MSGQ_VLD0);
    WRITE_REG(MSGQ_VLD0, g_ctrl_t[tid].msg_bitmap[0][0]);

    READ_REG(g_ctrl_t[tid].msg_bitmap[0][1], MSGQ_VLD1);
    WRITE_REG(MSGQ_VLD1, g_ctrl_t[tid].msg_bitmap[0][1]);

    READ_REG(g_ctrl_t[tid].msg_bitmap[1][0], MSGQ_VLD2);
    WRITE_REG(MSGQ_VLD2, g_ctrl_t[tid].msg_bitmap[1][0]);

    READ_REG(g_ctrl_t[tid].msg_bitmap[1][1], MSGQ_VLD3);
    WRITE_REG(MSGQ_VLD3, g_ctrl_t[tid].msg_bitmap[1][1]);
    #endif

    /* Free bits follow completion msgs on each track; then refresh MIX mask. */
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] |= g_ctrl_t[tid].msg_bitmap[i][j];
        }
    }
    set_mix(tid);
}

static inline uint32_t *map_ptr(ctrl_t *ctrl, int exe, int slot)
{
    return (slot == 0) ? ctrl->task_id_map1[exe] : ctrl->task_id_map2[exe];
}

/*
 * Drain one (exe, slot) msg bitmap into the completed-id list.
 *
 * MIX detection: both tracks at this core hold the same task_id.
 *   - both sides done (mix_side_done == 0x3) → enqueue id once
 *   - only one side done → do NOT enqueue; clear free bit again because
 *     read_msgq already OR-ed it free (stops CUBE/VECTOR reclaiming the core)
 * Non-MIX: enqueue immediately (same as pre-MIX get_completed).
 */
static inline void collect_completed(ctrl_t *ctrl, int exe, int slot,
                                     uint64_t *bitmap, uint32_t task_id[],
                                     int *complete_cnt)
{
    uint32_t *map = map_ptr(ctrl, exe, slot);
    uint32_t *other_map = map_ptr(ctrl, 1 - exe, slot);
    int cnt = __builtin_popcountll(*bitmap);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(*bitmap);
        uint64_t mask = (uint64_t)0x1 << idx;
        uint32_t tid = map[idx];
        uint32_t other_tid = other_map[idx];

        if (tid != 0 && other_tid == tid) {
            ctrl->mix_side_done[slot][idx] |= (uint8_t)(1u << exe);
            if (ctrl->mix_side_done[slot][idx] == 0x3) {
                task_id[(*complete_cnt)++] = tid;
                ctrl->mix_side_done[slot][idx] = 0;
                WORKER_LOGF("completed,mix,task_id,%u,complete_cnt,%d,core,%d,slot,%d",
                            tid, *complete_cnt, (int)idx, slot);
            } else {
                ctrl->free_bitmap[exe][slot] &= ~mask;
                WORKER_LOGF("completed,mix_partial,task_id,%u,side,%d,core,%d,slot,%d",
                            tid, exe, (int)idx, slot);
            }
        } else {
            task_id[(*complete_cnt)++] = tid;
            WORKER_LOGF("completed,task_id,%u,complete_cnt,%d,core,%d,slot,%d,exe,%d",
                        tid, *complete_cnt, (int)idx, slot, exe);
        }
        *bitmap &= (*bitmap - 1);
        cnt--;
    }
}

static inline void push_2_completed_queue(int tid)
{
    ctrl_t *ctrl = &g_ctrl_t[tid];
    uint32_t task_id[240];
    int complete_cnt = 0;
    for (int exe = 0; exe < EXE_TYPE_CNT; exe++) {
        collect_completed(ctrl, exe, 0, &ctrl->msg_bitmap[exe][0], task_id, &complete_cnt);
        collect_completed(ctrl, exe, 1, &ctrl->msg_bitmap[exe][1], task_id, &complete_cnt);
    }
    /* Partial MIX may have re-cleared free bits; keep free_bitmap[MIX] in sync. */
    set_mix(tid);
    if (complete_cnt > 0) {
        batch_enqueue(&ctrl->completed_queue, task_id, (uint32_t)complete_cnt);
        batch_enqueue(&ctrl->remote_completed_queue, task_id, (uint32_t)complete_cnt);
    }
}

/* Bind task_id onto one execution track (CUBE or VECTOR) at (slot, idx). */
static inline void bind_core(ctrl_t *ctrl, int exe, int slot, uint64_t idx,
                             uint32_t task_id)
{
    uint64_t mask = (uint64_t)0x1 << idx;
    if (slot == 1) {
        ctrl->task_id_map2[exe][idx] = task_id;
        #ifdef REAL_CHIP
        *ctrl->aicore_spr_2[exe][idx] = task_id;
        #endif
    } else {
        ctrl->task_id_map1[exe][idx] = task_id;
        #ifdef REAL_CHIP
        *ctrl->aicore_spr_1[exe][idx] = task_id;
        #endif
    }
    ctrl->free_bitmap[exe][slot] &= ~mask;
    #ifndef REAL_CHIP
    /* Fake Return: mark done immediately so sim can drain completed_queue. */
    ctrl->msg_bitmap[exe][slot] |= mask;
    #endif
}

static inline int send_task(ctrl_t *ctrl, int type)
{
    /*
     * Demand = free cores for this type:
     *   CUBE/VECTOR → that track's free bits
     *   MIX         → free_bitmap[MIX] (== cube & vector after set_mix)
     */
    uint64_t free_bitmap = (ctrl->free_bitmap[type][0] & ctrl->free_bitmap[type][1])
                          & ctrl->aicore_mask;
    int free_demand = __builtin_popcountll(free_bitmap);
    if (free_demand <= 0) {
        WORKER_LOGF("send,free_cnt,%d,type,%d", free_demand, type);
        return 0;
    }
    uint32_t task_ids[AIC_CNT];
    uint32_t got = (uint32_t)free_demand;
    if (!batch_dequeue(&ctrl->ready_queue[type], task_ids, &got)) {
        /* Cross-die work-stealing: same-type queue only (unchanged policy). */
        int stole = 0;
        for (uint32_t v = 0; v < DISPATCH_THREAD_CNT; v++) {
            if (v == ctrl->tid) {
                continue;
            }
            uint32_t want = (uint32_t)free_demand;
            if (batch_dequeue(&g_ctrl_t[v].ready_queue[type], task_ids, &want)) {
                got = want;
                WORKER_LOGF("steal,thief,%u,victim,%u,cnt,%u,type,%d",
                            ctrl->tid, v, got, type);
                stole = 1;
                break;
            }
        }
        if (!stole) {
            return 0;
        }
    }

    int sent = 0;
    for (uint32_t i = 0; i < got; i++) {
        uint32_t task_id = task_ids[i];
        uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
        uint64_t mask = (uint64_t)0x1 << idx;
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        int core = (int)idx;

        if (type == TASK_TYPE_MIX) {
            /* Dual-track occupy at the same index (implicit block). */
            ctrl->mix_side_done[slot][idx] = 0;
            bind_core(ctrl, TASK_TYPE_CUBE, slot, idx, task_id);
            bind_core(ctrl, TASK_TYPE_VECTOR, slot, idx, task_id);
            WORKER_LOGF("send,mix,task_id,%u,core,%d,slot,%d", task_id, core, slot);
        } else {
            bind_core(ctrl, type, slot, idx, task_id);
            WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
        }
        sent++;
        free_bitmap &= ~mask;
    }
    /* One refresh after the batch (CUBE/VECTOR free bits changed). */
    set_mix((int)ctrl->tid);
    return sent;
}

int dispatch(int tid)
{
    int total_sent = 0;
    read_msgq(tid);
    push_2_completed_queue(tid);
    /* MIX first: claim dual-free indices before pure CUBE/VECTOR consume a side. */
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    return total_sent;
}

void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    int total_sent = 0;
    WORKER_LOGF("dispatch,%d,start", tid);

    for (size_t i = 0; i < EXE_TYPE_CNT; i++)
    {
        hand_shake(tid, g_ctrl_t[tid].aicore_spr_1[i], i, 0);
        hand_shake(tid, g_ctrl_t[tid].aicore_spr_2[i], i, 64);
    }

    bool is_done = false;
    while (!is_done) {
        total_sent += dispatch(tid);
        is_done = atomic_load(&g_is_done);
    }
    WORKER_LOGF("dispatch,%d,done", tid);
    return NULL;
}
