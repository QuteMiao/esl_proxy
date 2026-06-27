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
 * @file swimlane_types.h
 * @brief L2 swimlane profiling data structures (level 1: AICore timing only)
 *
 * Shared memory layout (Host ↔ Device):
 * ┌─────────────────────────────────────────────────────────────┐
 * │ L2SwimlaneDataHeader                                        │
 * │  - per-thread ready queues                                  │
 * │  - num_cores, l2_swimlane_level (0=off, 1=AICore timing)    │
 * ├─────────────────────────────────────────────────────────────┤
 * │ L2SwimlaneAicoreTaskPool[0..num_cores-1]                    │
 * │  - head: active buffer + rotation seq / counters            │
 * │  - free_queue: SPSC ring of recycled AICore buffers         │
 * └─────────────────────────────────────────────────────────────┘
 *
 * L2SwimlaneAicoreTaskBuffer payloads are allocated by Host and linked
 * through each pool's free_queue. AICPU rotates buffers at dispatch
 * boundaries; AICore writes records; Host drains via the ready queue.
 *
 * Total size = sizeof(L2SwimlaneDataHeader)
 *            + num_cores * sizeof(L2SwimlaneAicoreTaskPool)
 */

#ifndef SRC_A2A3_PLATFORM_INCLUDE_COMMON_SWIMLANE_TYPES_H_
#define SRC_A2A3_PLATFORM_INCLUDE_COMMON_SWIMLANE_TYPES_H_

#include <assert.h>  /* static_assert in C11 */
#include <stddef.h>
#include <stdint.h>

#include "onboard_config.h"

// =============================================================================
// L2 swimlane_level — 0=disabled, 1=AICore per-task start/end timestamps.
// Transported via L2SwimlaneDataHeader::l2_swimlane_level (host → device).
// Wire values >1 are clamped to 1 by host/AICPU init.
// =============================================================================
typedef enum L2SwimlaneLevel {
    L2_SWIMLANE_LEVEL_DISABLED = 0,       // No collection
    L2_SWIMLANE_LEVEL_AICORE_TIMING = 1,  // AICore per-task start/end timestamps only
} L2SwimlaneLevel;

// =============================================================================
// L2SwimlaneAicoreTaskRecord - Slim AICore-Only Record (written by AICore, read by Host)
// =============================================================================

/**
 * Per-task record written by AICore into its per-core output buffer.
 * AICPU never reads or modifies this record at level 1.
 *
 * - `task_token_raw` — PTO2 task identity `(ring_id << 32) | local_id`.
 *   Canonical task id for host export / deps.json correlation. Not unique
 *   per dispatch when SPMD or pipeline reuses the same token on one core.
 *
 * - `reg_task_id` — per-core monotonic dispatch token (low 32 bits of
 *   dispatch_seq). Disambiguates multiple executions of the same
 *   task_token_raw on one core.
 *
 * Layout: 32B (half cache line). Two records pack per 64B line.
 */
typedef struct L2SwimlaneAicoreTaskRecord {
    uint64_t start_time;      // Task start timestamp (get_sys_cnt)
    uint64_t end_time;        // Task end timestamp
    uint64_t task_token_raw;  // PTO2TaskId::raw — identity (NOT join key)
    uint32_t reg_task_id;     // Per-core dispatch token — disambiguates re-dispatches
    uint32_t _pad;
} __attribute__((aligned(32))) L2SwimlaneAicoreTaskRecord;

static_assert(sizeof(L2SwimlaneAicoreTaskRecord) == 32, "L2SwimlaneAicoreTaskRecord must be 32B");

// =============================================================================
// TypedBuffer<Record, N> - Templated Fixed-Size Profiling Buffer
// =============================================================================

// Rotated per-core buffer pool: fixed capacity per buffer (power-of-two for
// cheap slot indexing) plus a free_queue of replacement buffers.
#define PLATFORM_AICORE_BUFFER_SIZE 1024
static_assert(
    (PLATFORM_AICORE_BUFFER_SIZE & (PLATFORM_AICORE_BUFFER_SIZE - 1)) == 0,
    "PLATFORM_AICORE_BUFFER_SIZE must be a power of two"
);

// PLATFORM_AICORE_BUFFERS_PER_CORE is declared in onboard_config.h.

/**
 * Fixed-capacity profiling buffer: record array + producer-written count.
 * (Concrete instantiation of the former TypedBuffer<Record, N> template.)
 */
typedef struct L2SwimlaneAicoreTaskBuffer {
    L2SwimlaneAicoreTaskRecord records[PLATFORM_AICORE_BUFFER_SIZE];
    volatile uint32_t count;
} __attribute__((aligned(64))) L2SwimlaneAicoreTaskBuffer;

// =============================================================================
// L2SwimlaneFreeQueue - SPSC Lock-Free Queue for Free Buffers
// =============================================================================

/**
 * Single Producer Single Consumer (SPSC) lock-free queue for free buffer management
 *
 * Producer: Host (ProfMemoryManager thread) pushes newly allocated buffers
 * Consumer: Device (AICPU thread) pops buffers when switching
 *
 * Queue semantics:
 * - Empty: head == tail
 * - Full: (tail - head) >= PLATFORM_PROF_SLOT_COUNT
 * - Capacity: PLATFORM_PROF_SLOT_COUNT buffers
 *
 * Memory ordering:
 * - Device pop: rmb() → read tail → read buffer_ptrs[head % COUNT] → rmb() → write head → wmb()
 * - Host push: write buffer_ptrs[tail % COUNT] → wmb() → write tail → wmb()
 */
typedef struct L2SwimlaneFreeQueue {
    volatile uint64_t buffer_ptrs[PLATFORM_PROF_SLOT_COUNT];  // Free buffer addresses
    volatile uint32_t head;                                   // Consumer read position (Device increments)
    volatile uint32_t tail;                                   // Producer write position (Host increments)
    uint32_t pad[13];                                         // Pad to 128 bytes (aligned to cache line)
} __attribute__((aligned(64))) L2SwimlaneFreeQueue;

static_assert(sizeof(L2SwimlaneFreeQueue) == 128, "L2SwimlaneFreeQueue must be 128 bytes for cache alignment");

// =============================================================================
// L2SwimlaneActiveHead - Shared "Active Buffer" Cache Line
// =============================================================================

/**
 * Single cache-line head describing the active buffer for an AICore task pool.
 *
 *   - current_buf_ptr      : buffer AICore is writing into (0 = none)
 *   - current_buf_seq      : rotation generation; AICore dcci-polls for bumps
 *   - total_record_count   : dispatches/records attempted (host reconcile)
 *   - dropped_record_count : records lost (queue full / no free buffer)
 *
 * AICPU is the sole writer; AICore reads only current_buf_ptr/current_buf_seq
 * via dcci. AICPU rotates before each new BUFFER_SIZE dispatch batch.
 */
typedef struct L2SwimlaneActiveHead {
    volatile uint64_t current_buf_ptr;       // 8 — active buffer device address (0 = none)
    volatile uint32_t current_buf_seq;       // 4 — monotonic seq / AICore rotation generation
    volatile uint32_t total_record_count;    // 4 — producer-attempted writes
    volatile uint32_t dropped_record_count;  // 4 — producer-dropped writes
    uint32_t pad[11];                        // 44 → 64B
} __attribute__((aligned(64))) L2SwimlaneActiveHead;

static_assert(sizeof(L2SwimlaneActiveHead) == 64, "L2SwimlaneActiveHead must be one cache line");

// =============================================================================
// L2SwimlaneAicoreTaskPool — ActiveHead (64B) + FreeQueue (128B) = 192B
// =============================================================================

/**
 * Per-core AICore task buffer pool.
 *
 *   head:       published to AICore via l2_swimlane_aicore_rotation_table[]
 *   free_queue: host pushes recycled buffers; AICPU pops on rotation
 *
 * AICPU enqueues full buffers to L2SwimlaneDataHeader ready queues
 * (ReadyQueueEntry::kind = AicoreTask). Host mgmt thread drains them.
 *
 * Rotation: AICPU counts dispatches per core; every PLATFORM_AICORE_BUFFER_SIZE
 * dispatches it swaps buffers before the next DATA_MAIN_BASE write.
 */
typedef struct L2SwimlaneAicoreTaskPool {
    L2SwimlaneActiveHead head;       // 64B
    L2SwimlaneFreeQueue free_queue;  // 128B
} __attribute__((aligned(64))) L2SwimlaneAicoreTaskPool;

static_assert(sizeof(L2SwimlaneAicoreTaskPool) == 192, "L2SwimlaneAicoreTaskPool must be 192 bytes");
// ABI lock: `&pool.head` is what AICPU publishes into the rotation_table for
// AICore to dcci. Must stay at offset 0 so AICore can index from KernelArgs.
static_assert(offsetof(L2SwimlaneAicoreTaskPool, head) == 0, "L2SwimlaneAicoreTaskPool::head must be at offset 0");
static_assert(
    offsetof(L2SwimlaneAicoreTaskPool, free_queue) == 64, "L2SwimlaneAicoreTaskPool::free_queue must be at offset 64"
);

// =============================================================================
// ReadyQueueEntry - Queue Entry for Ready Buffers
// =============================================================================

/** Buffer kind for ReadyQueueEntry::kind. Stored as uint32_t. */
typedef enum L2SwimlaneBufferKind {
    L2_SWIMLANE_BUFFER_KIND_AICORE_TASK = 0,  // Per-core L2SwimlaneAicoreTaskBuffer, AICore writes, AICPU enqueues
} L2SwimlaneBufferKind;

/**
 * Ready queue entry — AICPU pushes a full AICore buffer; Host drains it.
 */
typedef struct ReadyQueueEntry {
    uint32_t core_index;  // Core index (0 .. num_cores-1)
    uint32_t kind;        // Buffer kind discriminator (L2SwimlaneBufferKind value, fixed 4B for ABI)
    uint64_t buffer_ptr;  // Device pointer to the full buffer
    uint32_t buffer_seq;  // Sequence number for ordering
    uint32_t pad;         // Alignment padding
} __attribute__((aligned(32))) ReadyQueueEntry;

static_assert(sizeof(ReadyQueueEntry) == 32, "ReadyQueueEntry must be 32 bytes (host/device ABI)");

// =============================================================================
// L2SwimlaneDataHeader - Fixed Header
// =============================================================================

/**
 * Performance data fixed header
 *
 * Located at the start of shared memory, contains:
 * 1. Per-thread ready queues (FIFO Circular Buffers)
 * 2. Metadata (core count)
 *
 * Ready queue design:
 * - Per-thread queues: Avoid lock contention between AICPU threads
 * - Capacity per queue: PLATFORM_PROF_READYQUEUE_SIZE (full capacity for each thread)
 * - Implementation: Circular Buffer
 * - Producer: AICPU thread (adds full buffers to its own queue)
 * - Consumer: Host memory manager thread (reads from all queues)
 * - Queue empty: head == tail
 * - Queue full: (tail + 1) % capacity == head
 */
typedef struct L2SwimlaneDataHeader {
    // Per-thread ready queues (FIFO Circular Buffers)
    // Each AICPU thread has its own queue to avoid lock contention
    ReadyQueueEntry queues[PLATFORM_MAX_AICPU_THREADS][PLATFORM_PROF_READYQUEUE_SIZE];
    volatile uint32_t queue_heads[PLATFORM_MAX_AICPU_THREADS];  // Consumer read positions (Host modifies)
    volatile uint32_t queue_tails[PLATFORM_MAX_AICPU_THREADS];  // Producer write positions (AICPU modifies)

    // Metadata (Host initializes, Device read-only)
    uint32_t num_cores;          // Actual number of cores launched
    uint32_t l2_swimlane_level;  // 0=off, 1=AICore timing. Host writes at init.

    // Legacy header tail — kept for ABI; host init zeroes all fields below.
    uint32_t num_sched_phase_threads;
    uint32_t num_orch_phase_threads;
    uint32_t num_phase_cores;
    int8_t core_to_thread[PLATFORM_MAX_CORES];
} __attribute__((aligned(64))) L2SwimlaneDataHeader;

// ABI layout lock for the header tail (must not shift across host/device .so builds).
static_assert(
    offsetof(L2SwimlaneDataHeader, num_sched_phase_threads) ==
        offsetof(L2SwimlaneDataHeader, l2_swimlane_level) + sizeof(uint32_t),
    "L2SwimlaneDataHeader: num_sched_phase_threads must follow l2_swimlane_level"
);
static_assert(
    offsetof(L2SwimlaneDataHeader, num_orch_phase_threads) ==
        offsetof(L2SwimlaneDataHeader, num_sched_phase_threads) + sizeof(uint32_t),
    "L2SwimlaneDataHeader: num_orch_phase_threads must follow num_sched_phase_threads"
);
static_assert(
    offsetof(L2SwimlaneDataHeader, core_to_thread) ==
        offsetof(L2SwimlaneDataHeader, num_phase_cores) + sizeof(uint32_t),
    "L2SwimlaneDataHeader: core_to_thread[] must follow num_phase_cores"
);
static_assert(sizeof(L2SwimlaneDataHeader) % 64 == 0, "L2SwimlaneDataHeader must be 64-byte aligned");

// =============================================================================
// Helper Functions - Memory Layout
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

static inline size_t calc_l2_swimlane_data_size(int num_cores) {
    return sizeof(L2SwimlaneDataHeader) + (size_t)num_cores * sizeof(L2SwimlaneAicoreTaskPool);
}

static inline L2SwimlaneDataHeader *get_l2_swimlane_header(void *base_ptr) {
    return (L2SwimlaneDataHeader *)base_ptr;
}

static inline L2SwimlaneAicoreTaskPool *get_aicore_buffer_states(void *base_ptr, int num_cores) {
    (void)num_cores;
    return (L2SwimlaneAicoreTaskPool *)((char *)base_ptr + sizeof(L2SwimlaneDataHeader));
}

static inline L2SwimlaneAicoreTaskPool *get_aicore_buffer_state(void *base_ptr, int num_cores, int core_index) {
    (void)num_cores;
    return &get_aicore_buffer_states(base_ptr, core_index)[core_index];
}

#ifdef __cplusplus
}
#endif

#endif  // SRC_A2A3_PLATFORM_INCLUDE_COMMON_SWIMLANE_TYPES_H_
