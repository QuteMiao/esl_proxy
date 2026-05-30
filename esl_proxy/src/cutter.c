#include "cutter.h"
#include "log.h"
#include "ring_buf.h"

#include <stdint.h>

void cutter(queue_t *cq, queue_t *rq)
{
    uint16_t cq_buf[CUTTER_BATCH_SIZE];
    uint16_t rq_buf[AIC_CNT];
    uint16_t task_id;
    uint16_t succ_id;
    uint16_t idx;
    uint16_t succ_cnt;
    uint16_t ready_cnt = 0;

    uint16_t cnt = 0;
    if (batch_dequeue(cq, cq_buf, AIC_CNT)) {
        cnt = AIC_CNT;
    }
    for (uint32_t i = 0; i < cnt; i++) {
        task_id = cq_buf[i];
        idx = task_id & RING_MASK;
        task_state st = atomic_load_explicit(&g_state_buf[idx], memory_order_relaxed);
        succ_cnt = (uint16_t)st.successor_cnt;
        for (uint16_t j = 0; j < succ_cnt; j++) {
            succ_id = g_successor_buf[idx].successor[j];
            uint16_t left = (uint16_t)atomic_fetch_sub_explicit(
                &g_predecessor_buf[succ_id & RING_MASK], 1, memory_order_relaxed);
            if (left == 1) {
                rq_buf[ready_cnt++] = succ_id;
            }
        }
    }
    while (!batch_enqueue(rq, rq_buf, ready_cnt)) {
        spin_wait();
    }
    if (cnt > 0 || ready_cnt > 0) {
        WORKER_LOGF("cutter", "dequeued=%u ready=%u", cnt, ready_cnt);
    }
}

void *cutter_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    WORKER_LOGF("cutter", "worker %d started", tid);
    while (1) {
        spin_wait();
    }
    return NULL;
}
