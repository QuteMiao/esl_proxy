#include "cutter.h"
#include "log.h"
#include "ring_buf.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


task_state* g_state_buf;

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

static inline bool update_task_state(uint16_t cnt, uint16_t* cq_buf)
{
    uint16_t task_id;
    uint16_t idx;
    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        int idx = task_id & RING_MASK;
        g_state_buf[idx].state = TASK_STATUS_COMPLETED;
    }
    uint16_t i = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
    uint16_t end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    for (; i < (end & RING_MASK); i++) {
        if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
            break;
        }
    }
    atomic_store(&g_min_uncomplete_task, i);
}

void add_successors(uint16_t* ready_cnt, uint16_t* rq_buf) {
    uint16_t end = atomic_load(&g_task_id);
    while ( g_commit_task_id <= end)
    {
        uint16_t task_idx = g_commit_task_id & RING_MASK;
        struct predecessor_list *ptr = &g_predecessors[task_idx];
        if (ptr->cnt <= 0) {
            WORKER_LOGF("ready, task_id,%u, task_idx,%u, ready_cnt,%u", g_commit_task_id, task_idx, *ready_cnt);
            rq_buf[*ready_cnt] = g_commit_task_id++;
            (*ready_cnt)++;
            continue;
        }
        uint16_t precessor_id = 0;
        uint16_t predecessor_cnt = 0;
        while (ptr->cnt > 0)
        {
            precessor_id = *(ptr->exp);
            uint16_t precessor_idx = precessor_id & RING_MASK;
            // WORKER_LOGF("add, task_id,%u, precessor_id, %u", g_commit_task_id, precessor_id);
            if(g_state_buf[precessor_idx].state != TASK_STATUS_COMPLETED) {
                uint16_t successor_idx = g_successor_buf[precessor_idx].cnt++;
                g_successor_buf[precessor_idx].node[successor_idx] = g_commit_task_id;
                WORKER_LOGF("add, task_id,%u, successor_cnt,%u, successor_id, %u", precessor_id, g_successor_buf[precessor_idx].cnt, g_commit_task_id);
            }
            ptr->cnt--;
            ptr->exp++;
        }
        g_predecessor_cnt[task_idx] = predecessor_cnt;
        g_commit_task_id++;
    }
}

void send_2_ready_queue(int ready_cnt, uint16_t* rq_buf) {
    uint16_t task_id;
    for (uint16_t j = 0; j < ready_cnt; j++) {
        task_id = rq_buf[j];
        task_type_t type = g_basic_buf[task_id & RING_MASK].type;
        // int target_ctrl = task_id & (uint16_t)0x1;
        int target_ctrl = 0;
        queue_t *rq = &g_ctrl_t[target_ctrl].ready_queue[type];
        enqueue(rq, task_id);
        WORKER_LOGF("ready_queue,q_cnt,%d,task_id,%u,type,%d", rq->cnt, task_id, type);
    }
}

void resolve_dep(uint16_t cnt, uint16_t* cq_buf, uint16_t* rq_buf, uint16_t* ready_cnt) {
    uint16_t task_id;
    uint16_t succ_id;
    uint16_t idx;
    uint16_t succ_cnt;
    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        idx = task_id & RING_MASK;
        task_state st = g_state_buf[idx];
        succ_cnt = (uint16_t)st.successor_cnt;
        WORKER_LOGF("new,task_id,%u,type,%u, successor_cnt,%u", task_id, g_basic_buf[idx].type, succ_cnt);
        for (uint16_t k = 0; k < succ_cnt; k++) {
            succ_id = g_successor_buf[idx].node[k];
            
            uint16_t left = g_predecessor_cnt[succ_id & RING_MASK]--;
            WORKER_LOGF("cutter, task_id,%u, successor_id,%u, predecessor_cnt,%u", task_id, succ_id, left);
            if (left == 1) {
                rq_buf[*ready_cnt++] = succ_id;
            }
        }
    }
}

void deal_completed_queue() {
    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        uint16_t cq_buf[CUTTER_BATCH_SIZE];
        uint16_t rq_buf[AIC_CNT];
        uint16_t ready_cnt = 0;

        queue_t *cq = &g_ctrl_t[i].completed_queue;
        uint16_t cnt = 0;
        if (cq->cnt > 0) {
            batch_dequeue(cq, cq_buf, &cnt);
            update_task_state(cnt, cq_buf);
        }
        add_successors(&ready_cnt, rq_buf);
        resolve_dep(cnt, cq_buf, rq_buf, &ready_cnt);
        send_2_ready_queue(ready_cnt, rq_buf);
    }
}

void *cutter_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    init_state_buf();
    // while (!atomic_load(&g_is_done)) {
    //     deal_completed_queue();
    // }
    while (!atomic_load(&g_orch_is_done)) {
        deal_completed_queue();
    }

    while(g_commit_task_id < atomic_load(&g_task_id)){
        deal_completed_queue();
    }
    WORKER_LOGF("cutter, commit_tasks_cnt,%d ", g_commit_task_id);
    return NULL;
}