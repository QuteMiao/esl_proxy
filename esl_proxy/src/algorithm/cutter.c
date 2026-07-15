#include "cutter.h"
#include "dispatch.h"
#include "log.h"
#include "ring_buf.h"
#include "spin.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

task_state *g_state_buf;

void init_state_buf(void) {
    g_state_buf = malloc(sizeof(task_state) * RING_SIZE);
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_state_buf[i].state = TASK_STATUS_CREATING;
        g_state_buf[i].task_id = 0;
        g_state_buf[i].successor_cnt = 0;
    }
}

extern atomic_int g_min_uncomplete_task;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern _Atomic bool g_orch_is_done;
extern _Atomic bool g_is_done;

uint16_t  g_predecessor_cnt[RING_SIZE];
uint16_t g_commit_task_id = 0;
uint16_t g_completed_task_cnt = 0;

static inline bool update_task_state(uint16_t cnt, uint16_t* cq_buf)
{
    if (cnt <= 0)
        return false;

    uint16_t task_id;
    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        int idx = task_id;
        g_state_buf[idx].state = TASK_STATUS_COMPLETED;
    }
    uint16_t i = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
    uint16_t end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    for (; i < end; i++) {
        if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
            break;
        }
    }
    atomic_store(&g_min_uncomplete_task, i);
    WORKER_LOGF("min_uncomplete_task,%u, completed_cnt,%u, cube_ready_cnt,%d,vector_ready_cnt,%d", \
        g_min_uncomplete_task, end, g_ctrl_t[0].ready_queue[2].cnt, g_ctrl_t[0].ready_queue[1].cnt);
    return true;
}

void add_successors(uint16_t ready_cnt[], uint16_t rq_buf[][RQ_BATCH_SIZE]) {
    /* Half-open [g_commit_task_id, end): end is either commit+BATCH or g_task_id
     * (next free id). Must use '<' so we never commit an unpublished task_id.
     * (PR #13: Check loop exit — keep '<' vs old '<='.) */
    uint16_t end = atomic_load(&g_task_id);
    uint16_t tmp = g_commit_task_id + PRE_BATCH_SIZE;
    end = tmp > end ? end : tmp;
    while ( g_commit_task_id < end) {
        uint16_t task_idx = g_commit_task_id;
        struct predecessor_list *ptr = &g_predecessors[task_idx];
        if (ptr->cnt <= 0) {
            // WORKER_LOGF("ready, task_id,%u, task_idx,%u, ready_cnt,%u", g_commit_task_id, task_idx, *ready_cnt);
            task_type_t type = g_basic_buf[task_idx & RING_MASK].type;
            rq_buf[type][ready_cnt[type]] = g_commit_task_id;
            ready_cnt[type]++;
            WORKER_LOGF("ready_cnt[%d],%d", type, ready_cnt[type]);
            g_commit_task_id++;
            continue;
        }
        uint16_t precessor_id = 0;
        uint16_t predecessor_cnt = 0;
        while (ptr->cnt > 0)
        {
            precessor_id = *(ptr->exp);
            uint16_t precessor_idx = precessor_id;
            if (g_state_buf[precessor_idx].state != TASK_STATUS_COMPLETED) {
                uint16_t successor_idx = g_successor_buf[precessor_idx].cnt++;
                g_successor_buf[precessor_idx].node[successor_idx] = g_commit_task_id;
                g_state_buf[precessor_idx].successor_cnt++;
                predecessor_cnt++;
                WORKER_LOGF("add, task_id,%u, successor_cnt,%u, successor_id, %u", precessor_id, g_successor_buf[precessor_idx].cnt, g_commit_task_id);
            }
            ptr->cnt--;
            ptr->exp++;
        }
        g_predecessor_cnt[task_idx] = predecessor_cnt;
        if (predecessor_cnt <= 0) {
            task_type_t type = g_basic_buf[task_idx & RING_MASK].type;
            rq_buf[type][ready_cnt[type]] = g_commit_task_id;
            ready_cnt[type]++;
            WORKER_LOGF("ready_cnt[%d],%d", type, ready_cnt[type]);
        }
        g_commit_task_id++;
    }
}

void send_2_ready_queue(uint16_t ready_cnt[], uint16_t rq_buf[][RQ_BATCH_SIZE]) {
    for (uint16_t j = 0; j < 2; j++) {
        int target_ctrl = 0;
        queue_t *rq = &g_ctrl_t[target_ctrl].ready_queue[j];
        if (ready_cnt[j] > 0)
        {
            WORKER_LOGF("batch_enqueue,%d,cnt,%u,first,%d",j, ready_cnt[j], rq_buf[j][0]);
            batch_enqueue(rq, rq_buf[j], ready_cnt[j]);
        }
    }
}

void resolve_dep(uint16_t cnt, uint16_t* cq_buf, uint16_t rq_buf[][RQ_BATCH_SIZE], uint16_t* ready_cnt) {
    uint16_t task_id;
    uint16_t succ_id;
    uint16_t idx;
    uint16_t succ_cnt;
    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        idx = task_id & RING_MASK;
        task_state st = g_state_buf[idx];
        succ_cnt = (uint16_t)st.successor_cnt;
        WORKER_LOGF("completed,task_id,%u,type,%u, successor_cnt,%u", task_id, g_basic_buf[idx].type, succ_cnt);
        for (uint16_t k = 0; k < succ_cnt; k++) {
            succ_id = g_successor_buf[idx].node[k];
            g_predecessor_cnt[succ_id & RING_MASK]--;
            WORKER_LOGF("cutter, task_id,%u, successor_id,%u, predecessor_cnt,%u", task_id, succ_id, g_predecessor_cnt[succ_id & RING_MASK]);
            if (g_predecessor_cnt[succ_id & RING_MASK] < 1) {
                task_type_t type = g_basic_buf[succ_id].type;
                rq_buf[type][ready_cnt[type]] = succ_id;
                ready_cnt[type]++;
                WORKER_LOGF("ready_cnt[%d],%d", type, ready_cnt[type]);
            }
        }
    }
}

void deal_completed_queue() {
    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        uint16_t cq_buf[CQ_BATCH_SIZE];
        uint16_t rq_buf[2][RQ_BATCH_SIZE];
        uint16_t ready_cnt[2] = {0, 0};
        queue_t *cq = &g_ctrl_t[i].completed_queue;
        uint16_t cnt = CQ_BATCH_SIZE;
        batch_dequeue(cq, cq_buf, &cnt);
        g_completed_task_cnt += cnt;
        // for (size_t i = 0; i < cnt; i++)
        // {
        //     WORKER_LOGF("cutter, completed_task_id,%d ", cq_buf[i]);
        // }
        update_task_state(cnt, cq_buf);
        add_successors(ready_cnt, rq_buf);
        resolve_dep(cnt, cq_buf, rq_buf, ready_cnt);
        send_2_ready_queue(ready_cnt, rq_buf);
    }
}

void *cutter_worker(void *arg)
{
    (void)arg;
    init_state_buf();
    while (!atomic_load(&g_is_done)) {
        deal_completed_queue();
        spin_wait();
    }

    while (g_commit_task_id < atomic_load(&g_task_id)) {
        deal_completed_queue();
    }
    WORKER_LOGF("cutter, commit_tasks_cnt,%d,completed_task_cnt,%d ", g_commit_task_id, g_completed_task_cnt);
    return NULL;
}
