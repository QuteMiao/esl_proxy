#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>

#include "scheduler/painter.h"

task_state* g_state_buf[PAINTER_THREAD_CNT];
uint16_t commit_task_id[PAINTER_THREAD_CNT] = {0, 0};

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void init_state_buf(void) {
    for (int j = 0; j < PAINTER_THREAD_CNT; j++)
    {
        g_state_buf[j] = malloc(sizeof(task_state) * RING_SIZE);
        for (size_t i = 0; i < RING_SIZE; i++) {
            g_state_buf[j][i].state = TASK_STATUS_CREATING;
            g_state_buf[j][i].task_id = 0;
            g_state_buf[j][i].type = 0;
            g_state_buf[j][i].successor_cnt = 0;
        }
    }
}

extern atomic_int g_min_uncomplete_task;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern atomic_bool g_is_done;
uint16_t  g_predecessor_cnt[RING_SIZE];
uint16_t completed_task_cnt = 0;

static inline bool update_task_state(int tid, uint16_t cnt, uint16_t* cq_buf)
{
    if (cnt <= 0)
        return false;
    
    uint16_t task_id;
    uint16_t idx;

    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        int idx = task_id;
        g_state_buf[tid][idx].state = TASK_STATUS_COMPLETED;
    }

    if (tid == 0)
    {
        uint16_t i = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
        for (; i < total_task_cnt; i++) {
            if (g_state_buf[tid][i].state != TASK_STATUS_COMPLETED) {
                break;
            }
        }
        atomic_store(&g_min_uncomplete_task, i);
        printf("min_uncomplete_task,%u, total_task_cnt,%u, cube_ready_cnt,%d,vector_ready_cnt,%d\n", 
            g_min_uncomplete_task, total_task_cnt, g_ctrl_t[0].ready_queue[2].cnt, g_ctrl_t[0].ready_queue[1].cnt);
        completed_task_cnt += cnt;
        if (completed_task_cnt >= total_task_cnt)
            atomic_store_explicit(&g_is_done, true, memory_order_release);
    }
}

void add_successors(int tid, uint16_t ready_cnt[], uint16_t rq_buf[][RQ_BATCH_SIZE]) {
    uint16_t tmp = commit_task_id[tid] + PRE_BATCH_SIZE;
    uint16_t end = test_graph[tid].task_cnt - 1;
    end = tmp > end ? end : tmp;
    int commited_idx = commit_task_id[tid];
    while (commited_idx <= end)
    {
        uint16_t id = test_graph[tid].task_id[commited_idx];
        int pre_cnt = test_graph[tid].pre_cnt[commited_idx];
        task_type_t type = (task_type_t)test_graph[tid].type[commited_idx];
        g_state_buf[tid][id].type = type;
        if (pre_cnt <= 0) {
            rq_buf[type][ready_cnt[type]] = id;
            ready_cnt[type]++;
            printf("ready,task_id,%d,pre_cnt,%d, type,%d,cnt,%d,\n",id, pre_cnt, type, ready_cnt[type]);
            commited_idx++;
            continue;
        }
        uint16_t predecessor_cnt = 0;

        for (int i = 0; i < pre_cnt; i++)
        {
            int pre_idx = test_graph[tid].pre_idx[commited_idx] + i;
            uint16_t precessor_idx = (uint16_t)test_graph[tid].predecessors[pre_idx];
            
            if(g_state_buf[tid][precessor_idx].state != TASK_STATUS_COMPLETED) {
                uint16_t successor_idx = g_successor_buf[precessor_idx].cnt++;
                g_successor_buf[precessor_idx].node[successor_idx] = id;
                g_state_buf[tid][precessor_idx].successor_cnt++;
                predecessor_cnt++;
                printf("add,task_id,%u,successor_cnt,%u,successor_id,%u\n", precessor_idx, g_successor_buf[precessor_idx].cnt, id);
            }
        }
        g_predecessor_cnt[id] = predecessor_cnt;
        if (predecessor_cnt <= 0)
        {
            task_type_t type = test_graph[tid].type[commited_idx];
            rq_buf[type][ready_cnt[type]] = id;
            ready_cnt[type]++;
            printf("ready,type,%d,cnt,%d\n",type, ready_cnt[type]);
        }
        commited_idx++;
    }
    commit_task_id[tid] = commited_idx;
}

void send_2_ready_queue(uint16_t ready_cnt[], uint16_t rq_buf[][RQ_BATCH_SIZE]) {
    for (uint16_t j = 0; j < 2; j++) {
        int target_ctrl = 0;
        queue_t *rq = &g_ctrl_t[target_ctrl].ready_queue[j];
        if (ready_cnt[j] > 0)
        {
            printf("batch_enqueue,%d,cnt,%u,first,%d\n",j, ready_cnt[j], rq_buf[j][0]);
            batch_enqueue(rq, rq_buf[j], ready_cnt[j]);
        }
    }
}

void resolve_dep(int tid, uint16_t cnt, uint16_t* cq_buf, uint16_t rq_buf[][RQ_BATCH_SIZE], uint16_t* ready_cnt) {
    uint16_t task_id;
    uint16_t succ_id;
    uint16_t idx;
    uint16_t succ_cnt;

    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        idx = task_id & RING_MASK;
        task_state st = g_state_buf[tid][idx];
        succ_cnt = (uint16_t)st.successor_cnt;
        for (uint16_t k = 0; k < succ_cnt; k++) {
            succ_id = g_successor_buf[idx].node[k];
            g_predecessor_cnt[succ_id & RING_MASK]--;
            printf("painter,task_id,%u,successor_id,%u,predecessor_cnt,%u\n", task_id, succ_id, g_predecessor_cnt[succ_id & RING_MASK]);
            if (g_predecessor_cnt[succ_id & RING_MASK] < 1) {
                task_type_t type = g_state_buf[tid][succ_id].type;
                rq_buf[type][ready_cnt[type]] = succ_id;
                ready_cnt[type]++;
                printf("ready,task_id,%d,type,%d,cnt,%d\n",succ_id, type, ready_cnt[type]);
            }
        }
    }
}

void deal_completed_queue(int tid) {
    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        uint16_t cq_buf[CQ_BATCH_SIZE];
        uint16_t cnt = CQ_BATCH_SIZE;

        uint16_t rq_buf[2][RQ_BATCH_SIZE];
        uint16_t ready_cnt[2] = {0, 0};

        queue_t *cq = (tid == i) ? (&g_ctrl_t[i].completed_queue) : (&g_ctrl_t[i].remote_completed_queue);
        batch_dequeue(cq, cq_buf, &cnt);
        update_task_state(tid, cnt, cq_buf);
        add_successors(tid, ready_cnt, rq_buf);
        resolve_dep(tid, cnt, cq_buf, rq_buf, ready_cnt);
        send_2_ready_queue(ready_cnt, rq_buf);
    }
}

void buf_init(void)
{
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_successor_buf[i].next = NULL;
    }
}

void *painter(void *arg)
{
    int tid = (int)(intptr_t)arg;
    uint64_t start_ns = get_time_ns();
    printf("painter,%d,start\n", tid);
    bool is_done = false;
    while (!is_done) {
        deal_completed_queue(tid);
        is_done = atomic_load(&g_is_done);
    }
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;
    if (tid == 0)
    {
        printf("painter, commit_tasks_cnt,%d, completed_task_cnt,%d ", commit_task_id[tid], completed_task_cnt);
        printf("[painter] task_tp = %f MTasks/s",(float)(completed_task_cnt * 1000.0 / elapsed_ns));
    }
    printf("painter,%d,done\n", tid);
    return NULL;
}