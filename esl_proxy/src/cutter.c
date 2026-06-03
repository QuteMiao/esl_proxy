#include "cutter.h"
#include "log.h"
#include "ring_buf.h"

#include <stdint.h>

extern atomic_int g_task_id;
extern atomic_int g_completed_cnt;
extern ctrl_t g_ctrl_t[THREAD_CNT];

void *cutter_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    WORKER_LOGF("cutter", "worker %d started", tid);
    
    while (atomic_load(&g_completed_cnt) < atomic_load(&g_task_id)) {
        // 从所有 ctrl 的 completed_queue 取任务处理依赖
        for (int i = 0; i < THREAD_CNT; i++) {
            uint16_t cq_buf[CUTTER_BATCH_SIZE];
            uint16_t rq_buf[AIC_CNT];
            uint16_t task_id;
            uint16_t succ_id;
            uint16_t idx;
            uint16_t succ_cnt;
            uint16_t ready_cnt = 0;

            queue_t *cq = &g_ctrl_t[i].completed_queue;
            uint16_t cnt = 0;
            if (cq->cnt >= AIC_CNT) {
                lock_q(cq);
                if (batch_dequeue(cq, cq_buf, AIC_CNT)) {
                    cnt = AIC_CNT;
                }
                unlock_q(cq);
            }
            
            for (uint32_t j = 0; j < cnt; j++) {
                task_id = cq_buf[j];
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
            
            // 将ready的任务分发到对应类型的ready_queue
            for (uint16_t j = 0; j < ready_cnt; j++) {
                task_id = rq_buf[j];
                task_type_t type = g_basic_buf[task_id & RING_MASK].type;
                queue_t *rq = &g_ctrl_t[tid].ready_queue[type];
                lock_q(rq);
                enqueue(rq, task_id);
                unlock_q(rq);
            }
            
            if (cnt > 0 || ready_cnt > 0) {
                WORKER_LOGF("cutter", "tid=%d ctrl=%d dequeued=%u ready=%u", tid, i, cnt, ready_cnt);
            }
        }
        spin_wait();
    }
    
    WORKER_LOGF("cutter", "worker %d finished", tid);
    return NULL;
}