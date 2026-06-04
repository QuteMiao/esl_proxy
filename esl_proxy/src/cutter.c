#include "cutter.h"
#include "log.h"
#include "ring_buf.h"

#include <stdint.h>

extern atomic_int g_task_id;
extern atomic_int g_completed_cnt;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

void *cutter_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    WORKER_LOGF("worker %d started %d", tid, 0);
    
    while (atomic_load(&g_completed_cnt) < atomic_load(&g_task_id)) {
        // 从所有 ctrl 的 completed_queue 取任务处理依赖
        for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
            uint16_t cq_buf[CUTTER_BATCH_SIZE];
            uint16_t rq_buf[AIC_CNT];
            uint16_t task_id;
            uint16_t succ_id;
            uint16_t idx;
            uint16_t succ_cnt;
            uint16_t ready_cnt = 0;

            queue_t *cq = &g_ctrl_t[i].completed_queue;
            uint16_t cnt = CUTTER_BATCH_SIZE;
            // Process any available completed tasks (dequeue up to CUTTER_BATCH_SIZE at a time)
            if (cq->cnt > 0) 
                batch_dequeue(cq, cq_buf, &cnt);
            else
                continue;
            
            for (uint32_t j = 0; j < cnt; j++) {
                task_id = cq_buf[j];
                WORKER_LOGF("task_id=%u", task_id);
                idx = task_id & RING_MASK;
                task_state st = atomic_load_explicit(&g_state_buf[idx], memory_order_relaxed);
                succ_cnt = (uint16_t)st.successor_cnt;
                for (uint16_t k = 0; k < succ_cnt; k++) {
                    succ_id = g_successor_buf[idx].successor[k];
                    uint16_t left = (uint16_t)atomic_fetch_sub_explicit(
                        &g_predecessor_buf[succ_id & RING_MASK], 1, memory_order_relaxed);
                    if (left == 1) {
                        rq_buf[ready_cnt++] = succ_id;
                    }
                }
            }
            
            // 将ready的任务分发到对应类型的ready_queue (归属的ctrl: task_id & 1)
            for (uint16_t j = 0; j < ready_cnt; j++) {
                task_id = rq_buf[j];
                task_type_t type = g_basic_buf[task_id & RING_MASK].type;
                int target_ctrl = task_id & (uint16_t)0x1;
                queue_t *rq = &g_ctrl_t[target_ctrl].ready_queue[type];
                WORKER_LOGF("%d,%d,%u,to_rq,%d", tid, i, task_id, type);
                lock_q(rq);
                enqueue(rq, task_id);
                unlock_q(rq);
            }
            
        }
        spin_wait();
    }
    
    WORKER_LOGF("worker %d finished %d", tid, 0);
    return NULL;
}