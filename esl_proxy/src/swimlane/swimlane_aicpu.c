/*
 * Copyright (c) PyPTO Contributors.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 * -----------------------------------------------------------------------------------------------------------
 */

/**
 * @file swimlane_aicpu.c
 * @brief AICPU performance data collection implementation (SPSC free queue)
 *
 * Uses per-core L2SwimlaneAicoreTaskPool with SPSC free queues for O(1) buffer switching.
 * Host memory manager dynamically allocates replacement buffers and pushes
 * them into the free_queue. Device pops from free_queue when switching.
 */

#include "swimlane_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "memory_barrier.h"
#include "onboard_log.h"

void cache_civac_range(const void *addr, size_t size);

static L2SwimlaneDataHeader *s_l2_swimlane_header = NULL;

static L2SwimlaneAicoreTaskPool *s_aicore_task_pools[PLATFORM_MAX_CORES] = {0};
static uint32_t s_aicore_dispatched_count[PLATFORM_MAX_CORES] = {0};

static uint64_t g_platform_l2_swimlane_base = 0;
static bool g_enable_l2_swimlane = false;
static L2SwimlaneLevel g_l2_swimlane_level = L2_SWIMLANE_LEVEL_DISABLED;

// AICore rotation-table device pointer (= KernelArgs::l2_swimlane_aicore_rotation_table).
// Published by the host (sim: dlsym'd setter; onboard: from k_args via the
// kernel entry); AICPU init walks it to fill per-core &rotation addresses.
static uint64_t g_platform_l2_swimlane_aicore_rotation_table = 0;

void set_platform_l2_swimlane_base(uint64_t l2_swimlane_data_base) {
    g_platform_l2_swimlane_base = l2_swimlane_data_base;
}
uint64_t get_platform_l2_swimlane_base(void) { return g_platform_l2_swimlane_base; }
void set_l2_swimlane_enabled(bool enable) { g_enable_l2_swimlane = enable; }
bool is_l2_swimlane_enabled(void) { return g_enable_l2_swimlane; }
void set_platform_l2_swimlane_aicore_rotation_table(uint64_t table_addr) {
    g_platform_l2_swimlane_aicore_rotation_table = table_addr;
}
uint64_t get_platform_l2_swimlane_aicore_rotation_table(void) {
    return g_platform_l2_swimlane_aicore_rotation_table;
}

/**
 * Enqueue ready buffer to per-thread queue
 *
 * @param header L2SwimlaneDataHeader pointer
 * @param thread_idx AICPU thread index (selects the per-thread ready queue)
 * @param core_index Core index for AICore task entries
 * @param buffer_ptr Device pointer to the full buffer
 * @param buffer_seq Sequence number for ordering
 * @param kind Buffer kind discriminator (see L2SwimlaneBufferKind)
 * @return 0 on success, -1 if queue full
 */
static int enqueue_ready_buffer(
    L2SwimlaneDataHeader *header, int thread_idx, uint32_t core_index, uint64_t buffer_ptr, uint32_t buffer_seq,
    L2SwimlaneBufferKind kind
) {
    uint32_t capacity = PLATFORM_PROF_READYQUEUE_SIZE;
    uint32_t current_tail = header->queue_tails[thread_idx];
    uint32_t current_head = header->queue_heads[thread_idx];

    // Check if queue is full
    uint32_t next_tail = (current_tail + 1) % capacity;
    if (next_tail == current_head) {
        return -1;
    }

    header->queues[thread_idx][current_tail].core_index = core_index;
    header->queues[thread_idx][current_tail].kind = (uint32_t)kind;
    header->queues[thread_idx][current_tail].buffer_ptr = buffer_ptr;
    header->queues[thread_idx][current_tail].buffer_seq = buffer_seq;
    header->queue_tails[thread_idx] = next_tail;

    return 0;
}

void l2_swimlane_aicpu_init(int worker_count) {
    for (int i = 0; i < PLATFORM_MAX_CORES; i++) {
        s_aicore_dispatched_count[i] = 0;
    }

    void *l2_swimlane_base = (void *)g_platform_l2_swimlane_base;
    if (l2_swimlane_base == NULL) {
        LOG_ERROR("l2_swimlane_data_base is NULL, cannot initialize profiling");
        return;
    }

    s_l2_swimlane_header = get_l2_swimlane_header(l2_swimlane_base);
    g_l2_swimlane_level = (L2SwimlaneLevel)s_l2_swimlane_header->l2_swimlane_level;
    if (g_l2_swimlane_level != L2_SWIMLANE_LEVEL_DISABLED &&
        g_l2_swimlane_level != L2_SWIMLANE_LEVEL_AICORE_TIMING) {
        g_l2_swimlane_level = L2_SWIMLANE_LEVEL_AICORE_TIMING;
    }

    LOG_INFO_V0(
        "Initializing AICore swimlane for %d cores, l2_swimlane_level=%u", worker_count,
        (uint32_t)g_l2_swimlane_level);

    uint64_t *head_table = (uint64_t *)g_platform_l2_swimlane_aicore_rotation_table;

    for (int i = 0; i < worker_count; i++) {
        L2SwimlaneAicoreTaskPool *ac_state = get_aicore_buffer_state(l2_swimlane_base, worker_count, i);
        s_aicore_task_pools[i] = ac_state;

        if (head_table != NULL) {
            head_table[i] = (uint64_t)&ac_state->head;
        }

        rmb();
        uint32_t ac_head = ac_state->free_queue.head;
        uint32_t ac_tail = ac_state->free_queue.tail;
        if (ac_head != ac_tail) {
            uint64_t ac_buf_ptr = ac_state->free_queue.buffer_ptrs[ac_head % PLATFORM_PROF_SLOT_COUNT];
            rmb();
            ac_state->free_queue.head = ac_head + 1;
            ac_state->head.current_buf_ptr = ac_buf_ptr;
            wmb();
            ac_state->head.current_buf_seq = 0;
            wmb();
            L2SwimlaneAicoreTaskBuffer *ac_buf = (L2SwimlaneAicoreTaskBuffer *)ac_buf_ptr;
            ac_buf->count = 0;
            LOG_DEBUG("Core %d: primed AICore head with buf=0x%lx, seq=0", i, ac_buf_ptr);
        } else {
            LOG_ERROR("Core %d: AICore free_queue is empty during init!", i);
            ac_state->head.current_buf_ptr = 0;
            ac_state->head.current_buf_seq = 0;
            wmb();
        }
    }

    wmb();
    if (head_table != NULL && worker_count > 0) {
        /* head_table is a 64B-aligned GM base; round the size up to whole cache lines */
        size_t bytes = ((size_t)worker_count * sizeof(uint64_t) + 63u) & ~(size_t)63u;
        cache_civac_range(head_table, bytes);
    }

    LOG_INFO_V0("AICore swimlane initialized for %d cores", worker_count);
}

static void aicore_rotate(int core_id, int thread_idx) {
    L2SwimlaneAicoreTaskPool *ac_state = s_aicore_task_pools[core_id];
    if (ac_state == NULL) {
        return;
    }

    uint64_t old_buf_ptr = ac_state->head.current_buf_ptr;
    uint32_t seq = ac_state->head.current_buf_seq;

    rmb();
    uint32_t head = ac_state->free_queue.head;
    uint32_t tail = ac_state->free_queue.tail;
    if (head == tail) {
        // No replacement available — AICore continues to write into the old
        // buffer; its slot counter will hit BUFFER_SIZE and the slot guard
        // silently drops further records. We deliberately do NOT bump
        // dropped_record_count here: AICPU has no precise view of how many
        // tasks will actually fall in this gap before the run ends. The
        // pre-emptive BUFFER_SIZE bump that used to live here over-counted
        // when the run ended early — the old buffer's already-written
        // records still flushed (counted toward `collected`), and the
        // pre-emptive bump on top of that broke the
        // `collected + dropped == total` reconcile invariant. The drop is
        // visible at reconcile time as silent loss
        // (`total - collected - dropped > 0`) and the WARN below records
        // the failure mode.
        LOG_WARN(
            "Thread %d: Core %d AICore free_queue empty at rotation; AICore slot guard will drop overflow records",
            thread_idx, core_id
        );
        return;
    }

    // Enqueue the just-filled AICore buffer with count = BUFFER_SIZE.
    if (old_buf_ptr != 0) {
        L2SwimlaneAicoreTaskBuffer *old_buf = (L2SwimlaneAicoreTaskBuffer *)old_buf_ptr;
        old_buf->count = (uint32_t)PLATFORM_AICORE_BUFFER_SIZE;
        wmb();
        int rc = enqueue_ready_buffer(
            s_l2_swimlane_header, thread_idx, core_id, old_buf_ptr, seq, L2_SWIMLANE_BUFFER_KIND_AICORE_TASK
        );
        if (rc != 0) {
            // Ready queue full — we leave current_buf_ptr pointing at the
            // old buffer so the run-end flush path retries the enqueue (the
            // host is draining concurrently; the queue may have space by
            // then). We deliberately do NOT bump dropped here for the same
            // reason as the empty-free-queue branch: counting a drop now
            // would double-count if the flush succeeds in delivering the
            // buffer to the host. Reconcile reports the actual loss as
            // silent_loss when neither this rotation nor the flush
            // delivers the records.
            LOG_ERROR(
                "Thread %d: Core %d failed to enqueue AICore buffer at rotation (queue full); will retry at flush",
                thread_idx, core_id
            );
            return;
        }
    }

    // Pop next buffer from free_queue and publish via the head channel.
    // Publish order matters: AICore observes head.current_buf_seq change to
    // detect rotation, then reads head.current_buf_ptr. Write ptr first so
    // AICore can never see a new seq with a stale ptr. new_buf->count=0 must
    // also be visible before AICore's slot writes begin.
    uint64_t new_buf_ptr = ac_state->free_queue.buffer_ptrs[head % PLATFORM_PROF_SLOT_COUNT];
    rmb();
    ac_state->free_queue.head = head + 1;
    L2SwimlaneAicoreTaskBuffer *new_buf = (L2SwimlaneAicoreTaskBuffer *)new_buf_ptr;
    new_buf->count = 0;

    wmb();
    ac_state->head.current_buf_ptr = new_buf_ptr;
    wmb();
    ac_state->head.current_buf_seq = seq + 1;
    wmb();
}

// Pre-dispatch hook. Called from the dispatch path (scheduler_dispatch in
// tensormap_and_ringbuffer; aicpu_executor in host_build_graph) immediately
// before `write_reg(DATA_MAIN_BASE)` for each AICore task. Maintains the
// per-core dispatch count and rotates the AICore buffer when the count is
// about to cross a PLATFORM_AICORE_BUFFER_SIZE boundary.
//
// Race safety: rotation runs before the dispatch register write. The
// completion-before-dispatch invariant (AICore per core is single-threaded
// and AICPU does not dispatch task K+1 until K FIN'd) guarantees AICore has
// already finished writing — and dcci'd out — every record in the old buffer
// by then. AICPU can safely enqueue the old buffer to the ready queue.
//
// total_record_count accounting also lives here: one AICore record == one
// dispatch, so the dispatch count IS the AICore-side total. Bumping here
void l2_swimlane_aicpu_on_aicore_dispatch(int core_id, int thread_idx) {
    if (!g_enable_l2_swimlane) {
        return;
    }
    if (core_id < 0 || core_id >= PLATFORM_MAX_CORES) {
        return;
    }
    L2SwimlaneAicoreTaskPool *ac_state = s_aicore_task_pools[core_id];
    if (ac_state == NULL) {
        return;
    }
    uint32_t prev = s_aicore_dispatched_count[core_id];
    // Rotate exactly on the first dispatch of each non-initial BUFFER_SIZE
    // batch (prev = BUFFER_SIZE, 2*BUFFER_SIZE, ...). PLATFORM_AICORE_BUFFER_SIZE
    // is asserted power-of-two so the mod lowers to a bitwise AND.
    if (prev > 0 && (prev & (PLATFORM_AICORE_BUFFER_SIZE - 1)) == 0) {
        aicore_rotate(core_id, thread_idx);
    }
    s_aicore_dispatched_count[core_id] = prev + 1;
    ac_state->head.total_record_count += 1;
}

void l2_swimlane_aicpu_flush(int thread_idx, const int *cur_thread_cores, int core_num) {
    if (!g_enable_l2_swimlane) {
        return;
    }

    void *l2_swimlane_base = (void *)g_platform_l2_swimlane_base;
    if (l2_swimlane_base == NULL) {
        return;
    }

    rmb();

    LOG_INFO_V0("Thread %d: Flushing performance buffers for %d cores", thread_idx, core_num);

    int flushed_count = 0;

    for (int i = 0; i < core_num; i++) {
        int core_id = cur_thread_cores[i];
        L2SwimlaneAicoreTaskPool *ac_state = s_aicore_task_pools[core_id];
        if (ac_state == NULL) continue;

        rmb();
        uint64_t ac_buf_ptr = ac_state->head.current_buf_ptr;
        if (ac_buf_ptr == 0) continue;

        uint32_t live = ac_state->head.total_record_count -
                        ac_state->head.current_buf_seq * (uint32_t)PLATFORM_AICORE_BUFFER_SIZE;
        if (live == 0) {
            continue;
        }
        uint32_t ac_mark = (live > (uint32_t)PLATFORM_AICORE_BUFFER_SIZE) ?
                               (uint32_t)PLATFORM_AICORE_BUFFER_SIZE :
                               live;
        L2SwimlaneAicoreTaskBuffer *ac_buf = (L2SwimlaneAicoreTaskBuffer *)ac_buf_ptr;
        ac_buf->count = ac_mark;
        wmb();

        uint32_t ac_seq = ac_state->head.current_buf_seq;
        int rc = enqueue_ready_buffer(
            s_l2_swimlane_header, thread_idx, core_id, ac_buf_ptr, ac_seq, L2_SWIMLANE_BUFFER_KIND_AICORE_TASK
        );
        if (rc == 0) {
            LOG_INFO_V0(
                "Thread %d: Core %d flushed AICore buffer (seq=%u, count=%u)", thread_idx, core_id, ac_seq, ac_mark
            );
            flushed_count++;
            ac_state->head.current_buf_ptr = 0;
            wmb();
        } else {
            LOG_ERROR("Thread %d: Core %d failed to enqueue AICore buffer at flush (queue full)", thread_idx, core_id);
            ac_state->head.dropped_record_count = ac_state->head.dropped_record_count + ac_mark;
            ac_state->head.current_buf_ptr = 0;
            wmb();
        }
    }

    wmb();

    LOG_INFO_V0("Thread %d: Performance buffer flush complete, %d buffers flushed", thread_idx, flushed_count);
}
