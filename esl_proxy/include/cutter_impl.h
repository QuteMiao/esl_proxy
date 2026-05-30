/*
 * cutter_impl.h - Cutter internal implementation
 *
 * All implementation is static inline in this header.
 * Include cutter.h before this file.
 *
 * Naming follows Constitution XI: concise names within cutter module context.
 */

#ifndef DAG_CUTTER_IMPL_H
#define DAG_CUTTER_IMPL_H

#include "conf.h"
#include "cutter.h"
#include "task.h"
#include "shmem_layout.h"

/*
 * Thread-local remaining predecessor count array
 * Built once during init, used during dependency resolution
 */
static _Thread_local uint32_t g_remaining_pred[RING_SIZE];

/*
 * Init implementation
 */
static inline cutter_ctx_t *cutter_init(const cutter_cfg_t *restrict cfg) {
    if (cfg->notify_fn == NULL) return NULL;

    cutter_ctx_t *ctx = (cutter_ctx_t *)cfg->shmem_completed; /* alias ctx in available memory */
    ctx->shmem_completed  = (completed_region_t *restrict)cfg->shmem_completed;
    ctx->shmem_dag        = (dag_region_t *restrict)cfg->shmem_dag;
    ctx->shmem_successors = (struct successorList *restrict)cfg->shmem_successors;
    ctx->shmem_ready      = (ready_region_t *restrict)cfg->shmem_ready;
    ctx->notify_fn        = cfg->notify_fn;
    ctx->notify_userdata  = cfg->notify_userdata;
    ctx->ready_count      = 0;

    return ctx;
}


/*
 * Read completed task IDs from completed queue region
 * Returns number of task IDs written to buf
 */
static inline uint32_t cutter_read_complete(cutter_ctx_t *restrict ctx,
                                            uint16_t *restrict buf,
                                            uint32_t max) {
    completed_region_t *restrict reg = ctx->shmem_completed;
    queue_t *restrict q = &reg->queue;

    uint64_t cnt = atomic_queue_count(q);
    if (cnt == 0) return 0;

    uint64_t tail = atomic_read_queue_tail(q);
    uint32_t actual = (uint32_t)(cnt < max ? cnt : max);

    uint32_t written = 0;
    for (uint32_t i = 0; i < actual && written < max; i++) {
        uint64_t pos = (tail + i) & RING_MASK;
        uint16_t tid = q->tasks[pos];
        /* Verify task is completed */
        if (atomic_task_state(reg->g_state_buf, tid) == COMPLETED) {
            buf[written++] = tid;
        }
    }

    return written;
}

/*
 * Walk successor list for a given task
 * Calls the provided block for each successor
 *
 * Usage:
 *   CUTTER_FOR_EACH_SUCCESSOR(ctx, task_id, {
 *       // s is the successor task_id
 *       process(s);
 *   });
 */
#define CUTTER_FOR_EACH_SUCCESSOR(ctx, task_id, body) \
    do { \
        struct successorList *_succ = &(ctx)->shmem_successors[(task_id)]; \
        while (_succ) { \
            for (int _i = 0; _i < 3; _i++) { \
                uint16_t s = _succ->successor[_i]; \
                if (s == 0) break; \
                body; \
            } \
            _succ = _succ->next; \
        } \
    } while (0)

/*
 * Build remaining predecessor count array from DAG
 * Stores in-degree per task in thread-local g_remaining_pred[]
 */
static inline void cutter_build_pred_array(cutter_ctx_t *restrict ctx) {
    dag_region_t *restrict dag = ctx->shmem_dag;
    uint32_t node_count = dag->node_count;
    (void)dag;

    for (uint32_t i = 0; i < node_count; i++) {
        g_remaining_pred[i] = 0;
    }

    /* Count predecessors for each task (build in-degree) */
    for (uint32_t i = 0; i < node_count; i++) {
        CUTTER_FOR_EACH_SUCCESSOR(ctx, i, {
            g_remaining_pred[s]++;
        });
    }
}

/*
 * Resolve dependencies for a completed task
 * Walks successors, decrements their predecessor counts,
 * and adds any task reaching 0 to the ready batch
 */
static inline void cutter_resolve(cutter_ctx_t *restrict ctx, uint16_t task_id) {
    CUTTER_FOR_EACH_SUCCESSOR(ctx, task_id, {
        /* Atomically decrement predecessor count */
        uint32_t prev = atomic_decr_successor_cnt(
            ctx->shmem_completed->g_state_buf, s);

        /* If just reached 0, task s is now ready */
        if (prev == 1) {
            if (ctx->ready_count < CUTTER_MAX_READY_BATCH) {
                ctx->ready_batch[ctx->ready_count++] = s;
            }
            if (ctx->notify_fn) {
                ctx->notify_fn(&s, 1, ctx->notify_userdata);
            }
        }
    });
}

/*
 * Write ready task notifications to shared memory
 * Enqueues all collected ready task IDs and increments seq_num
 */
static inline void cutter_write_ready(cutter_ctx_t *restrict ctx) {
    if (ctx->ready_count == 0) return;

    ready_region_t *restrict reg = ctx->shmem_ready;
    queue_t *restrict q = &reg->ready_queue;

    for (uint32_t i = 0; i < ctx->ready_count; i++) {
        atomic_queue_enqueue(q, ctx->ready_batch[i]);
    }

    /* Signal Dispatch with seq_num increment */
    uint64_t prev = atomic_load_explicit(&reg->seq_num, memory_order_relaxed);
    atomic_store_explicit(&reg->seq_num, prev + 1, memory_order_release);

    ctx->ready_count = 0; /* reset batch after writing */
}

#endif /* DAG_CUTTER_IMPL_H */
