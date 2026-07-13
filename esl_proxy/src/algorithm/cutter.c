#include "cutter.h"
#include "dispatch.h"
#include "log.h"
#include "memory_barrier.h"
#include "ring_buf.h"
#include "spin.h"
#include "platform.h"

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

void init_state_buf(void) {
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
extern atomic_flag g_fanout_lock[RING_SIZE];

_Atomic uint16_t g_predecessor_cnt[RING_SIZE];
_Atomic uint16_t g_commit_task_id = 0;
_Atomic uint16_t g_completed_task_cnt = 0;

/* Per-producer fanout lock: held only while touching one producer's successor
 * list / completion state, so different producers proceed in parallel. */
static inline void lock_fanout(uint16_t p)
{
    while (atomic_flag_test_and_set_explicit(&g_fanout_lock[p & RING_MASK], memory_order_acquire)) {
        spin_wait();
    }
}

static inline void unlock_fanout(uint16_t p)
{
    atomic_flag_clear_explicit(&g_fanout_lock[p & RING_MASK], memory_order_release);
}

/* Advance the ring back-pressure watermark past the completed prefix. Multiple
 * cutters may run this concurrently; it only ever reads COMPLETED marks (set
 * under the producer's fanout lock), so it never advances past a genuinely
 * incomplete task — a stale read merely stops early (conservative). */
static inline void advance_min_uncomplete(void)
{
    uint16_t i = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
    uint16_t end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    for (; i < end; i++) {
        if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
            break;
        }
    }
    atomic_store_explicit(&g_min_uncomplete_task, (int)i, memory_order_release);
    wmb();
}

/* Mark a newly-ready successor and stage it for routing. */
static inline void stage_ready(uint16_t succ_id, uint16_t ready_cnt[], uint16_t rq_buf[][LOCAL_BUFFER_SIZE])
{
    task_type_t type = g_basic_buf[succ_id & RING_MASK].type;
    dispatch_spmd_on_ready(succ_id);
    rq_buf[type][ready_cnt[type]++] = succ_id;
    WORKER_LOGF("ready_cnt[%d],%d", (int)type, ready_cnt[type]);
}

/* Wire the DAG for the next batch of committed tasks (lane 0 only). For each
 * task S, prepend S onto every still-live predecessor's fanout list under that
 * predecessor's lock; predecessors already COMPLETED at wire time are accounted
 * immediately. The declared predecessor count is stored BEFORE any wiring so a
 * concurrent predecessor completion can safely fetch_sub against it (count-down
 * to zero == ready). Per-producer locking makes the wire-vs-resolve of a given
 * predecessor mutually exclusive, closing the build-vs-resolve race. */
void add_successors(uint16_t ready_cnt[], uint16_t rq_buf[][LOCAL_BUFFER_SIZE]) {
    uint16_t end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    uint16_t commit = atomic_load_explicit(&g_commit_task_id, memory_order_acquire);
    uint16_t tmp = (uint16_t)(commit + ADD_BATCH_SIZE);
    end = tmp > end ? end : tmp;
    while (commit < end) {
        uint16_t task_idx = commit;
        struct predecessor_list *ptr = &g_predecessors[task_idx];
        uint16_t decl = ptr->cnt;

        /* Publish the target count first; only afterwards may a predecessor's
         * completion (or our own self-decrement below) fetch_sub against it. */
        atomic_store_explicit(&g_predecessor_cnt[task_idx], decl, memory_order_release);
        wmb();

        if (decl == 0) {
            stage_ready(commit, ready_cnt, rq_buf);
            commit++;
            atomic_store_explicit(&g_commit_task_id, commit, memory_order_release);
            continue;
        }

        for (uint16_t k = 0; k < decl; k++) {
            uint16_t p = ptr->exp[k];
            bool completed;

            lock_fanout(p);
            completed = (g_state_buf[p & RING_MASK].state == TASK_STATUS_COMPLETED);
            if (!completed) {
                uint16_t si = g_successor_buf[p & RING_MASK].cnt++;
                g_successor_buf[p & RING_MASK].node[si] = commit;
                g_state_buf[p & RING_MASK].successor_cnt++;
                WORKER_LOGF("add, task_id,%u, successor_cnt,%u, successor_id, %u",
                            p, g_successor_buf[p & RING_MASK].cnt, commit);
            }
            unlock_fanout(p);

            if (completed) {
                /* Already done and thus not in its fanout list — account for it
                 * now; it will never fire a completion-driven decrement for us. */
                uint16_t prev = atomic_fetch_sub_explicit(&g_predecessor_cnt[task_idx], 1,
                                                          memory_order_acq_rel);
                if (prev == 1) {
                    stage_ready(commit, ready_cnt, rq_buf);
                }
            }
        }
        ptr->cnt = 0;
        commit++;
        atomic_store_explicit(&g_commit_task_id, commit, memory_order_release);
    }
}

/* Route newly-ready tasks straight into the single shared per-shape ready pool
 * (g_shared_ready[shape]); every dispatch lane pulls its own free-core count from
 * it, so a starving lane always finds queued work. Aligns with simpler, which has
 * no per-thread local queue — all ready tasks live in the shared shape queues
 * (scheduler_dispatch.cpp). `tid` retained for the call-site signature only. */
void send_2_ready_queue(int tid, uint16_t ready_cnt[], uint16_t rq_buf[][LOCAL_BUFFER_SIZE]) {
    (void)tid;
    for (uint16_t j = 0; j < TASK_TYPE_CNT; j++) {
        uint16_t n = ready_cnt[j];
        if (n == 0) {
            continue;
        }
        WORKER_LOGF("batch_enqueue,%d,cnt,%u,first,%d,shared", j, n, rq_buf[j][0]);
        batch_enqueue(&g_shared_ready[j], rq_buf[j], n);
    }
}

/* For each just-completed producer P: mark it COMPLETED and snapshot its fanout
 * list under P's lock (mutually exclusive with add_successors appending to it),
 * then atomically count-down each successor. Whichever decrement drives a
 * successor's predecessor count to zero (here or a self-decrement in
 * add_successors) makes it ready — exactly once. */
void resolve_dep(uint16_t cnt, uint16_t* cq_buf, uint16_t rq_buf[][LOCAL_BUFFER_SIZE], uint16_t* ready_cnt) {
    uint16_t succ_snap[CON_NODE_CNT];

    for (uint32_t j = 0; j < cnt; j++) {
        uint16_t task_id = cq_buf[j];
        uint16_t idx = task_id & RING_MASK;
        uint16_t succ_cnt;

        lock_fanout(task_id);
        g_state_buf[idx].state = TASK_STATUS_COMPLETED;
        succ_cnt = (uint16_t)g_state_buf[idx].successor_cnt;
        if (succ_cnt > CON_NODE_CNT) {
            succ_cnt = CON_NODE_CNT;
        }
        for (uint16_t k = 0; k < succ_cnt; k++) {
            succ_snap[k] = g_successor_buf[idx].node[k];
        }
        unlock_fanout(task_id);

        WORKER_LOGF("completed,task_id,%u,type,%u, successor_cnt,%u", task_id, g_basic_buf[idx].type, succ_cnt);
        for (uint16_t k = 0; k < succ_cnt; k++) {
            uint16_t succ_id = succ_snap[k];
            uint16_t prev = atomic_fetch_sub_explicit(&g_predecessor_cnt[succ_id & RING_MASK], 1,
                                                      memory_order_acq_rel);
            WORKER_LOGF("cutter, task_id,%u, successor_id,%u, predecessor_cnt,%u",
                        task_id, succ_id, (unsigned)(prev - 1));
            if (prev == 1) {
                stage_ready(succ_id, ready_cnt, rq_buf);
            }
        }
    }
}

/* Lane `tid` drains ONLY its own completed_queue. DAG mutation is protected by
 * per-producer fanout locks (not a global lock), so lanes resolve different
 * completions in parallel. `resolve_dep` runs on every lane (marking its own
 * completions and counting down successors); `add_successors` — the global
 * commit-cursor wiring pass — stays on lane 0. resolve_dep runs before
 * add_successors so a predecessor completed in this batch is visible (as
 * COMPLETED, under its lock) when lane 0 wires that predecessor's successors. */
void deal_completed_queue(int tid) {
    uint16_t cq_buf[CUTTER_BATCH_SIZE];
    uint16_t cnt = CUTTER_BATCH_SIZE;
    queue_t *cq = &g_ctrl_t[tid].completed_queue;

    batch_dequeue(cq, cq_buf, &cnt);

    uint16_t rq_buf[TASK_TYPE_CNT][LOCAL_BUFFER_SIZE];
    uint16_t ready_cnt[TASK_TYPE_CNT] = {0, 0, 0};

    atomic_fetch_add_explicit(&g_completed_task_cnt, cnt, memory_order_relaxed);
    resolve_dep(cnt, cq_buf, rq_buf, ready_cnt);
    if (tid == 0) {
        add_successors(ready_cnt, rq_buf);
    }
    advance_min_uncomplete();
    send_2_ready_queue(tid, ready_cnt, rq_buf);
}

void *cutter_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;

    /* g_state_buf lives in BSS (zeroed); init_state_buf only rewrites the
     * benign CREATING/EMPTY distinction, so a concurrent read during lane 0's
     * init is harmless (only COMPLETED is load-bearing, and nothing is
     * COMPLETED until dispatch runs). */
    if (tid == 0) {
        init_state_buf();
    }

    while (!atomic_load_explicit(&g_is_done, memory_order_relaxed)) {
        deal_completed_queue(tid);
        spin_wait();
    }
    /* Post-shutdown drain: flush any residual completions and, for lane 0,
     * finish advancing the commit cursor. */
    for (;;) {
        deal_completed_queue(tid);
        bool cq_empty = (g_ctrl_t[tid].completed_queue.cnt == 0);
        bool commit_done = (tid != 0) ||
            (atomic_load_explicit(&g_commit_task_id, memory_order_acquire) >=
             atomic_load_explicit(&g_task_id, memory_order_acquire));
        if (cq_empty && commit_done) {
            break;
        }
    }
    WORKER_LOGF("cutter, tid,%d, commit_tasks_cnt,%d,completed_task_cnt,%d ", tid, g_commit_task_id, g_completed_task_cnt);
    return NULL;
}