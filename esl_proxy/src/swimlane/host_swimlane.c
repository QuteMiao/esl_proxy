/* Host-side L2 swimlane collector (pure C, a2a3 SVM). C ABI for host_onboard.c.
 *
 * This file folds in what used to be the template collector framework
 * (swimlane_collector_base.h + swimlane_collector.h). With a single concrete
 * collector and the a2a3 SVM path, the generic BufferPoolManager<Module> /
 * ProfilerBase<Derived,Module> templates collapse into the concrete static
 * functions below. Threading uses pthreads (matching the rest of the runner);
 * the mgmt/poll loops, buffer pool, ready/done queues, JSON export, and
 * reconcile all live here.
 */
#define _POSIX_C_SOURCE 200809L

#include "swimlane_host.h"

#include "kernel_args.h"
#include "runtime.h"
#include "swimlane_types.h"
#include "memory_barrier.h"
#include "onboard_log.h"

#include <sys/time.h>  /* struct timeval — must precede ascend_hal.h */

#include <acl/acl_rt.h>
#include <ascend_hal.h>
#include <dlfcn.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// AICore-task is the only buffer kind at level 1.
enum { PROF_BUFFER_TYPE_AICORE_TASK = 0 };

// Idle-timeout / replenish batch (former L2SwimlaneModule / collector contract).
#define SW_IDLE_TIMEOUT_SEC PLATFORM_PROF_TIMEOUT_SECONDS
#define SW_READYQUEUE_SIZE PLATFORM_PROF_READYQUEUE_SIZE
#define SW_SLOT_COUNT PLATFORM_PROF_SLOT_COUNT

// =============================================================================
// Types
// =============================================================================

typedef void *(*sw_alloc_fn)(size_t size);
typedef int (*sw_free_fn)(void *dev_ptr);
typedef int (*sw_register_fn)(void *dev_ptr, size_t size, int device_id, void **host_ptr_out);
typedef int (*sw_unregister_fn)(void *host_ptr, int device_id);

// Full (rotated) buffer handed from the mgmt thread to the poll thread.
typedef struct {
    int type;               // PROF_BUFFER_TYPE_*
    uint32_t index;         // core_index
    uint32_t slot_idx;      // reserved (free-queue design)
    void *dev_buffer_ptr;   // device address of the full buffer
    void *host_buffer_ptr;  // host-mapped address (a2a3 SVM: same as dev)
    uint32_t buffer_seq;    // sequence number for ordering
} ReadyBufferInfo;

typedef struct {
    void *dev;
    void *host;
} DevHostMap;

typedef struct {
    L2SwimlaneAicoreTaskRecord *data;
    size_t size;
    size_t cap;
} RecordVec;

typedef struct {
    // -- memory context (set by initialize()) --
    sw_alloc_fn alloc_cb;
    sw_register_fn register_cb;  // NULL => identity-map (host == dev)
    sw_free_fn free_cb;
    void *shm_dev;
    void *shm_host;  // also the is_initialized() flag
    size_t shm_size;
    int device_id;

    // -- dev->host mapping (single source of truth for resolve) --
    DevHostMap *map;
    size_t map_size, map_cap;

    // -- recycled pool (single kind) --
    void **recycled;
    size_t rec_size, rec_cap;

    // -- ready queue: mgmt thread pushes, poll thread pops --
    ReadyBufferInfo *ready;
    size_t ready_cap, ready_head, ready_count;
    pthread_mutex_t ready_mtx;
    pthread_cond_t ready_cv;

    // -- done queue: poll thread reports, mgmt thread recycles (single kind) --
    void **done;
    size_t done_cap, done_head, done_count;
    pthread_mutex_t done_mtx;

    // -- threads --
    pthread_t mgmt_thread, poll_thread;
    atomic_bool mgmt_running;
    atomic_bool execution_complete;
    bool started;

    // -- L2 collector state --
    void *perf_shared_mem_dev;
    void *aicore_ring_addr_table_dev;
    int num_aicore;
    int aicpu_thread_num;
    L2SwimlaneLevel level;
    int32_t *core_types;  // 0=AIC, 1=AIV
    int core_types_count;
    char output_prefix[PATH_MAX];
    RecordVec *records;  // [num_aicore]
    uint64_t total_aicore_collected;
} L2Collector;

static L2Collector g;

// =============================================================================
// dev->host map / recycled pool
// =============================================================================

static void map_put(void *dev, void *host) {
    if (g.map_size == g.map_cap) {
        size_t nc = g.map_cap ? g.map_cap * 2 : 128;
        g.map = (DevHostMap *)realloc(g.map, nc * sizeof(*g.map));
        g.map_cap = nc;
    }
    g.map[g.map_size].dev = dev;
    g.map[g.map_size].host = host;
    g.map_size++;
}

static void *map_resolve(void *dev) {
    for (size_t i = 0; i < g.map_size; i++) {
        if (g.map[i].dev == dev) return g.map[i].host;
    }
    LOG_ERROR("BufferPool: no host mapping for dev_ptr=%p", dev);
    return NULL;
}

static void map_erase(void *dev) {
    for (size_t i = 0; i < g.map_size; i++) {
        if (g.map[i].dev == dev) {
            g.map[i] = g.map[g.map_size - 1];
            g.map_size--;
            return;
        }
    }
}

static void recycled_push(void *dev) {
    if (g.rec_size == g.rec_cap) {
        size_t nc = g.rec_cap ? g.rec_cap * 2 : 128;
        g.recycled = (void **)realloc(g.recycled, nc * sizeof(*g.recycled));
        g.rec_cap = nc;
    }
    g.recycled[g.rec_size++] = dev;
}

static void *recycled_pop(void) {
    return g.rec_size ? g.recycled[--g.rec_size] : NULL;
}

// Allocate a device buffer + paired host view, recording the mapping. On a2a3
// SVM the register callback (halHostRegister) returns the identity-mapped host
// view; with no register_cb the host pointer is the device pointer.
static void *alloc_and_register(size_t size, void **host_ptr_out) {
    void *dev = g.alloc_cb(size);
    if (dev == NULL) {
        *host_ptr_out = NULL;
        return NULL;
    }
    void *host = NULL;
    if (g.register_cb != NULL) {
        int rc = g.register_cb(dev, size, g.device_id, &host);
        if (rc != 0 || host == NULL) {
            LOG_ERROR("BufferPool: register failed: %d", rc);
            if (g.free_cb) g.free_cb(dev);
            *host_ptr_out = NULL;
            return NULL;
        }
    } else {
        host = dev;
    }
    *host_ptr_out = host;
    map_put(dev, host);
    return dev;
}

// =============================================================================
// ready queue (mutex + cond) / done queue (mutex)
// =============================================================================

static void ready_reserve(size_t need) {
    if (need <= g.ready_cap) return;
    size_t nc = g.ready_cap ? g.ready_cap * 2 : 64;
    while (nc < need) nc *= 2;
    ReadyBufferInfo *nb = (ReadyBufferInfo *)malloc(nc * sizeof(*nb));
    for (size_t i = 0; i < g.ready_count; i++) {
        nb[i] = g.ready[(g.ready_head + i) % g.ready_cap];
    }
    free(g.ready);
    g.ready = nb;
    g.ready_cap = nc;
    g.ready_head = 0;
}

static void ready_push(const ReadyBufferInfo *info) {
    pthread_mutex_lock(&g.ready_mtx);
    ready_reserve(g.ready_count + 1);
    g.ready[(g.ready_head + g.ready_count) % g.ready_cap] = *info;
    g.ready_count++;
    pthread_cond_signal(&g.ready_cv);
    pthread_mutex_unlock(&g.ready_mtx);
}

static bool ready_try_pop(ReadyBufferInfo *out) {
    bool ok = false;
    pthread_mutex_lock(&g.ready_mtx);
    if (g.ready_count) {
        *out = g.ready[g.ready_head];
        g.ready_head = (g.ready_head + 1) % g.ready_cap;
        g.ready_count--;
        ok = true;
    }
    pthread_mutex_unlock(&g.ready_mtx);
    return ok;
}

static bool ready_wait_pop(ReadyBufferInfo *out, long timeout_ms) {
    bool ok = false;
    pthread_mutex_lock(&g.ready_mtx);
    if (g.ready_count == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g.ready_cv, &g.ready_mtx, &ts);
    }
    if (g.ready_count) {
        *out = g.ready[g.ready_head];
        g.ready_head = (g.ready_head + 1) % g.ready_cap;
        g.ready_count--;
        ok = true;
    }
    pthread_mutex_unlock(&g.ready_mtx);
    return ok;
}

static void done_reserve(size_t need) {
    if (need <= g.done_cap) return;
    size_t nc = g.done_cap ? g.done_cap * 2 : 64;
    while (nc < need) nc *= 2;
    void **nb = (void **)malloc(nc * sizeof(*nb));
    for (size_t i = 0; i < g.done_count; i++) {
        nb[i] = g.done[(g.done_head + i) % g.done_cap];
    }
    free(g.done);
    g.done = nb;
    g.done_cap = nc;
    g.done_head = 0;
}

static void done_push(void *dev_ptr) {
    pthread_mutex_lock(&g.done_mtx);
    done_reserve(g.done_count + 1);
    g.done[(g.done_head + g.done_count) % g.done_cap] = dev_ptr;
    g.done_count++;
    pthread_mutex_unlock(&g.done_mtx);
}

static void drain_done_into_recycled(void) {
    pthread_mutex_lock(&g.done_mtx);
    while (g.done_count) {
        void *p = g.done[g.done_head];
        g.done_head = (g.done_head + 1) % g.done_cap;
        g.done_count--;
        recycled_push(p);
    }
    pthread_mutex_unlock(&g.done_mtx);
}

// =============================================================================
// mgmt-loop algorithms (former ProfilerAlgorithms<L2SwimlaneModule>)
// =============================================================================

// Three-level fallback used by process_entry's 1-in/1-out replenish.
static void *obtain_buffer(size_t buf_size) {
    void *p = recycled_pop();
    if (p != NULL) return p;
    drain_done_into_recycled();
    p = recycled_pop();
    if (p != NULL) return p;

    void *host = NULL;
    p = alloc_and_register(buf_size, &host);
    if (p == NULL) {
        LOG_WARN("L2Swimlane: alloc failed for %zu bytes — increase BUFFERS_PER_* to reduce drops", buf_size);
    }
    return p;
}

// On a2a3 SVM `fq` aliases device memory; publish the slot before the tail
// (wmb() between) so AICPU never sees a tail bump without the pointer.
static void push_to_free_queue(L2SwimlaneFreeQueue *fq, void *dev_ptr) {
    uint32_t fq_tail = fq->tail;
    uint32_t slot_idx = fq_tail % SW_SLOT_COUNT;
    fq->buffer_ptrs[slot_idx] = (uint64_t)dev_ptr;
    wmb();
    fq->tail = fq_tail + 1;
    wmb();
}

static void top_up_free_queue(L2SwimlaneFreeQueue *fq, size_t buf_size) {
    rmb();
    uint32_t fq_head = fq->head;
    uint32_t fq_tail = fq->tail;
    uint32_t fq_used = fq_tail - fq_head;

    while (fq_used < SW_SLOT_COUNT) {
        void *new_dev = recycled_pop();
        if (new_dev == NULL) {
            int batch = PLATFORM_AICORE_BUFFERS_PER_CORE - SW_SLOT_COUNT;
            if (batch < 1) batch = 1;
            for (int i = 0; i < batch; i++) {
                void *host = NULL;
                void *dev = alloc_and_register(buf_size, &host);
                if (dev == NULL) break;
                recycled_push(dev);
            }
            new_dev = recycled_pop();
        }
        if (new_dev == NULL) return;

        uint32_t slot_idx = fq_tail % SW_SLOT_COUNT;
        fq->buffer_ptrs[slot_idx] = (uint64_t)new_dev;
        wmb();
        fq_tail++;
        fq->tail = fq_tail;
        wmb();
        fq_used++;
    }
}

// Pop one entry from the per-thread ready queue. On a2a3 SVM `header` aliases
// device shared memory, so head advance and entry read hit live memory; the
// rmb() orders the entry load after the head!=tail check and the buffer_ptr==0
// guard skips a pre-publish entry until the producer finishes.
static bool try_pop_aicpu_entry(L2SwimlaneDataHeader *header, int q, ReadyQueueEntry *out) {
    uint32_t head = header->queue_heads[q];
    uint32_t tail = header->queue_tails[q];
    if (head >= SW_READYQUEUE_SIZE || tail >= SW_READYQUEUE_SIZE) {
        LOG_ERROR(
            "L2Swimlane: invalid queue indices for thread %d: head=%u tail=%u (max=%u)", q, head, tail,
            (uint32_t)SW_READYQUEUE_SIZE
        );
        return false;
    }
    if (head == tail) return false;
    rmb();
    *out = header->queues[q][head];
    if (out->buffer_ptr == 0) {
        return false;
    }
    head = (head + 1) % SW_READYQUEUE_SIZE;
    header->queue_heads[q] = head;
    wmb();
    return true;
}

static void copy_aicore_buffer(const ReadyBufferInfo *info);

// Refill the originating pool's free_queue with one buffer, then deliver the
// popped buffer to the collector. Drops (no deliver) on invalid kind/core or
// failed host resolution.
static void process_entry(L2SwimlaneDataHeader *header, int q, const ReadyQueueEntry *entry) {
    (void)q;
    int num_cores = (int)header->num_cores;
    if (entry->kind != L2_SWIMLANE_BUFFER_KIND_AICORE_TASK) {
        LOG_ERROR("L2Swimlane: invalid entry kind=%u", entry->kind);
        return;
    }
    if (entry->core_index >= (uint32_t)num_cores) {
        LOG_ERROR("L2Swimlane: invalid AICore entry: core=%u", entry->core_index);
        return;
    }

    L2SwimlaneAicoreTaskPool *ac = get_aicore_buffer_state(g.shm_host, num_cores, (int)entry->core_index);
    size_t buffer_size = sizeof(L2SwimlaneAicoreTaskBuffer);

    void *new_dev = obtain_buffer(buffer_size);
    if (new_dev != NULL) {
        push_to_free_queue(&ac->free_queue, new_dev);
    }

    ReadyBufferInfo info;
    info.type = PROF_BUFFER_TYPE_AICORE_TASK;
    info.index = entry->core_index;
    info.slot_idx = 0;
    info.dev_buffer_ptr = (void *)entry->buffer_ptr;
    info.buffer_seq = entry->buffer_seq;
    info.host_buffer_ptr = map_resolve(info.dev_buffer_ptr);
    if (info.host_buffer_ptr == NULL) {
        return;  // resolve logged; drop rather than deliver null.
    }
    ready_push(&info);
}

static void proactive_replenish(L2SwimlaneDataHeader *header) {
    drain_done_into_recycled();
    int num_cores = (int)header->num_cores;
    for (int i = 0; i < num_cores; i++) {
        L2SwimlaneAicoreTaskPool *ac = get_aicore_buffer_state(g.shm_host, num_cores, i);
        top_up_free_queue(&ac->free_queue, sizeof(L2SwimlaneAicoreTaskBuffer));
    }
}

// =============================================================================
// threads (former ProfilerBase mgmt/poll loops)
// =============================================================================

static void mgmt_loop(void) {
    L2SwimlaneDataHeader *header = get_l2_swimlane_header(g.shm_host);

    while (atomic_load_explicit(&g.mgmt_running, memory_order_acquire)) {
        drain_done_into_recycled();

        bool found_any = false;
        for (int q = 0; q < PLATFORM_MAX_AICPU_THREADS; q++) {
            ReadyQueueEntry entry;
            while (try_pop_aicpu_entry(header, q, &entry)) {
                process_entry(header, q, &entry);
                found_any = true;
            }
        }

        proactive_replenish(header);

        if (!found_any) {
            struct timespec ts = {0, 10000};  // 10 us
            nanosleep(&ts, NULL);
        }
    }

    // Final drain after mgmt_running flipped: don't sleep, don't replenish.
    for (int q = 0; q < PLATFORM_MAX_AICPU_THREADS; q++) {
        ReadyQueueEntry entry;
        while (try_pop_aicpu_entry(header, q, &entry)) {
            process_entry(header, q, &entry);
        }
    }
}

static void consume(const ReadyBufferInfo *info) {
    copy_aicore_buffer(info);
    done_push(info->dev_buffer_ptr);
}

static void poll_and_collect_loop(void) {
    const long tick_ms = 100;
    struct timespec idle_start = {0, 0};
    bool idling = false;

    for (;;) {
        ReadyBufferInfo info;
        if (ready_wait_pop(&info, tick_ms)) {
            consume(&info);
            idling = false;
            continue;
        }
        if (atomic_load_explicit(&g.execution_complete, memory_order_acquire)) {
            while (ready_try_pop(&info)) {
                consume(&info);
            }
            break;
        }
        if (!idling) {
            clock_gettime(CLOCK_MONOTONIC, &idle_start);
            idling = true;
        } else {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - idle_start.tv_sec >= SW_IDLE_TIMEOUT_SEC) {
                LOG_ERROR("L2Swimlane collector idle timeout after %d seconds — giving up", SW_IDLE_TIMEOUT_SEC);
                break;
            }
        }
    }
}

static void *mgmt_thread_entry(void *arg) {
    (void)arg;
    if (aclrtSetDevice(g.device_id) != ACL_SUCCESS) {
        fprintf(stderr, "[esl_proxy] swimlane mgmt thread aclrtSetDevice(%d) failed\n", g.device_id);
        return NULL;
    }
    mgmt_loop();
    return NULL;
}

static void *poll_thread_entry(void *arg) {
    (void)arg;
    if (aclrtSetDevice(g.device_id) != ACL_SUCCESS) {
        fprintf(stderr, "[esl_proxy] swimlane poll thread aclrtSetDevice(%d) failed\n", g.device_id);
        return NULL;
    }
    poll_and_collect_loop();
    return NULL;
}

// Order matters: mgmt is started before poll because mgmt is the only writer
// to the ready_queue and poll is its sole consumer.
static void collector_start(void) {
    if (g.shm_host == NULL) return;
    atomic_store_explicit(&g.mgmt_running, true, memory_order_release);
    pthread_create(&g.mgmt_thread, NULL, mgmt_thread_entry, NULL);
    atomic_store_explicit(&g.execution_complete, false, memory_order_release);
    pthread_create(&g.poll_thread, NULL, poll_thread_entry, NULL);
    g.started = true;
}

// Stop mgmt first so its final-drain entries have a consumer. Idempotent.
static void collector_stop(void) {
    if (!g.started) return;
    atomic_store_explicit(&g.mgmt_running, false, memory_order_release);
    pthread_join(g.mgmt_thread, NULL);
    atomic_store_explicit(&g.execution_complete, true, memory_order_release);
    pthread_join(g.poll_thread, NULL);
    g.started = false;
}

// =============================================================================
// teardown helpers
// =============================================================================

static void release_one_buffer(void *dev_ptr, sw_unregister_fn unregister_cb, sw_free_fn free_cb) {
    if (dev_ptr == NULL) return;
    if (unregister_cb != NULL) {
        int rc = unregister_cb(dev_ptr, g.device_id);
        if (rc != 0) {
            LOG_ERROR("halHostUnregister failed for dev_ptr %p: %d", dev_ptr, rc);
        }
    }
    if (free_cb) {
        free_cb(dev_ptr);
    }
}

// Release framework-owned buffers (recycled + done + ready). Safe only after
// collector_stop() has joined both threads (no concurrency, each buffer lives
// in exactly one container).
static void release_owned_buffers(sw_unregister_fn unregister_cb, sw_free_fn free_cb) {
    for (size_t i = 0; i < g.rec_size; i++) {
        release_one_buffer(g.recycled[i], unregister_cb, free_cb);
        map_erase(g.recycled[i]);
    }
    g.rec_size = 0;

    while (g.done_count) {
        void *p = g.done[g.done_head];
        g.done_head = (g.done_head + 1) % g.done_cap;
        g.done_count--;
        release_one_buffer(p, unregister_cb, free_cb);
        map_erase(p);
    }
    while (g.ready_count) {
        void *p = g.ready[g.ready_head].dev_buffer_ptr;
        g.ready_head = (g.ready_head + 1) % g.ready_cap;
        g.ready_count--;
        release_one_buffer(p, unregister_cb, free_cb);
        map_erase(p);
    }
}

static void drain_free_queue(L2SwimlaneFreeQueue *fq, sw_unregister_fn unregister_cb, sw_free_fn free_cb) {
    rmb();
    uint32_t head = fq->head;
    uint32_t tail = fq->tail;
    uint32_t queued = tail - head;
    if (queued > SW_SLOT_COUNT) {
        queued = SW_SLOT_COUNT;
    }
    for (uint32_t k = 0; k < queued; k++) {
        uint32_t slot = (head + k) % SW_SLOT_COUNT;
        release_one_buffer((void *)fq->buffer_ptrs[slot], unregister_cb, free_cb);
        fq->buffer_ptrs[slot] = 0;
    }
    fq->head = tail;
}

// =============================================================================
// L2 collector: init / collect / reconcile / export / finalize
// =============================================================================

static int collector_initialize(
    int num_aicore, int aicpu_thread_num, int device_id, L2SwimlaneLevel level, sw_alloc_fn alloc_cb,
    sw_register_fn register_cb, sw_free_fn free_cb, const char *output_prefix
) {
    if (g.shm_host != NULL) {
        LOG_ERROR("L2SwimlaneCollector already initialized");
        return -1;
    }

    LOG_INFO_V0("Initializing performance profiling");

    if (num_aicore <= 0 || num_aicore > PLATFORM_MAX_CORES) {
        LOG_ERROR("Invalid number of AICores: %d (max=%d)", num_aicore, PLATFORM_MAX_CORES);
        return -1;
    }

    g.num_aicore = num_aicore;
    g.aicpu_thread_num = aicpu_thread_num;
    g.level = level;
    snprintf(g.output_prefix, sizeof(g.output_prefix), "%s",
             (output_prefix != NULL && output_prefix[0] != '\0') ? output_prefix : ".");
    g.total_aicore_collected = 0;

    // Stash the memory context up front so alloc_and_register sees consistent
    // values; shm_host stays NULL until allocation succeeds (start() no-ops
    // on a post-failure call).
    g.alloc_cb = alloc_cb;
    g.register_cb = register_cb;
    g.free_cb = free_cb;
    g.shm_dev = NULL;
    g.shm_host = NULL;
    g.shm_size = 0;
    g.device_id = device_id;

    pthread_mutex_init(&g.ready_mtx, NULL);
    pthread_cond_init(&g.ready_cv, NULL);
    pthread_mutex_init(&g.done_mtx, NULL);
    atomic_init(&g.mgmt_running, false);
    atomic_init(&g.execution_complete, false);
    g.started = false;

    size_t total_size = calc_l2_swimlane_data_size(num_aicore);

    LOG_DEBUG("Shared memory allocation plan:");
    LOG_DEBUG("  Number of cores:      %d", num_aicore);
    LOG_DEBUG("  Header size:          %zu bytes", sizeof(L2SwimlaneDataHeader));
    LOG_DEBUG("  L2SwimlaneAicoreTaskPool size: %zu bytes each", sizeof(L2SwimlaneAicoreTaskPool));
    LOG_DEBUG("  Total shared memory:  %zu bytes (%zu KB)", total_size, total_size / 1024);

    void *perf_dev_ptr = alloc_cb(total_size);
    if (perf_dev_ptr == NULL) {
        LOG_ERROR("Failed to allocate shared memory (%zu bytes)", total_size);
        return -1;
    }
    LOG_DEBUG("Allocated shared memory: %p", perf_dev_ptr);

    void *perf_host_ptr = NULL;
    if (register_cb != NULL) {
        int rc = register_cb(perf_dev_ptr, total_size, device_id, &perf_host_ptr);
        if (rc != 0) {
            LOG_ERROR("Memory registration failed: %d", rc);
            return rc;
        }
        if (perf_host_ptr == NULL) {
            LOG_ERROR("register_cb succeeded but returned null host_ptr");
            return -1;
        }
        LOG_DEBUG("Mapped to host memory: %p", perf_host_ptr);
    } else {
        perf_host_ptr = perf_dev_ptr;
        LOG_DEBUG("Simulation mode: host_ptr = dev_ptr = %p", perf_host_ptr);
    }

    L2SwimlaneDataHeader *header = get_l2_swimlane_header(perf_host_ptr);
    for (int t = 0; t < PLATFORM_MAX_AICPU_THREADS; t++) {
        memset(header->queues[t], 0, sizeof(header->queues[t]));
        header->queue_heads[t] = 0;
        header->queue_tails[t] = 0;
    }
    header->num_cores = num_aicore;
    header->l2_swimlane_level = (uint32_t)level;
    header->num_sched_phase_threads = 0;
    header->num_orch_phase_threads = 0;
    header->num_phase_cores = 0;
    memset(header->core_to_thread, -1, sizeof(header->core_to_thread));

    LOG_DEBUG("Initialized L2SwimlaneDataHeader:");
    LOG_DEBUG("  num_cores:              %d", header->num_cores);
    LOG_DEBUG("  l2_swimlane_level: %u", header->l2_swimlane_level);
    LOG_DEBUG("  aicore_buffer_capacity: %d", PLATFORM_AICORE_BUFFER_SIZE);
    LOG_DEBUG("  queue capacity:         %d", PLATFORM_PROF_READYQUEUE_SIZE);

    g.records = (RecordVec *)calloc((size_t)num_aicore, sizeof(RecordVec));

    // Per-core AICore rotation channel + buffer pool.
    for (int i = 0; i < num_aicore; i++) {
        L2SwimlaneAicoreTaskPool *ac_state = get_aicore_buffer_state(perf_host_ptr, num_aicore, i);
        memset(ac_state, 0, sizeof(L2SwimlaneAicoreTaskPool));

        for (int s = 0; s < PLATFORM_AICORE_BUFFERS_PER_CORE; s++) {
            void *host_buf_ptr = NULL;
            void *dev_buf_ptr = alloc_and_register(sizeof(L2SwimlaneAicoreTaskBuffer), &host_buf_ptr);
            if (dev_buf_ptr == NULL) {
                LOG_ERROR("Failed to allocate L2SwimlaneAicoreTaskBuffer for core %d, buffer %d", i, s);
                return -1;
            }
            L2SwimlaneAicoreTaskBuffer *buf = (L2SwimlaneAicoreTaskBuffer *)host_buf_ptr;
            memset(buf, 0, sizeof(L2SwimlaneAicoreTaskBuffer));
            buf->count = 0;

            if (s == 0) {
                ac_state->free_queue.buffer_ptrs[0] = (uint64_t)dev_buf_ptr;
            } else {
                recycled_push(dev_buf_ptr);
            }
        }
        wmb();
        ac_state->free_queue.tail = 1;
        wmb();
    }
    LOG_DEBUG(
        "Initialized buffer pools: %d L2SwimlaneAicoreTaskBuffers/core (1 in free_queue, rest in recycled pool)",
        PLATFORM_AICORE_BUFFERS_PER_CORE
    );

    // Standalone uint64_t[num_aicore] rotation table (AICPU fills the entries).
    {
        size_t table_bytes = (size_t)num_aicore * sizeof(uint64_t);
        void *rotation_table_host = NULL;
        void *rotation_table_dev = alloc_and_register(table_bytes, &rotation_table_host);
        if (rotation_table_dev == NULL) {
            LOG_ERROR("Failed to allocate l2_swimlane_aicore_rotation_table (rotation) table (%zu bytes)", table_bytes);
            return -1;
        }
        g.aicore_ring_addr_table_dev = rotation_table_dev;
    }

    wmb();

    LOG_DEBUG("L2 swimlane device base = 0x%lx", (unsigned long)(uintptr_t)perf_dev_ptr);

    g.perf_shared_mem_dev = perf_dev_ptr;
    // Publish the now-known SHM tuple; start() gates on shm_host.
    g.shm_dev = perf_dev_ptr;
    g.shm_host = perf_host_ptr;
    g.shm_size = total_size;

    LOG_INFO_V0("Performance profiling initialized (dynamic buffer mode)");
    return 0;
}

static void recvec_push(RecordVec *v, const L2SwimlaneAicoreTaskRecord *r) {
    if (v->size == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 64;
        v->data = (L2SwimlaneAicoreTaskRecord *)realloc(v->data, nc * sizeof(*v->data));
        v->cap = nc;
    }
    v->data[v->size++] = *r;
}

static void copy_aicore_buffer(const ReadyBufferInfo *info) {
    L2SwimlaneAicoreTaskBuffer *buf = (L2SwimlaneAicoreTaskBuffer *)info->host_buffer_ptr;
    rmb();
    uint32_t core_index = info->index;
    if (core_index >= (uint32_t)g.num_aicore) {
        return;
    }
    uint32_t count = buf->count;
    if (count > (uint32_t)PLATFORM_AICORE_BUFFER_SIZE) {
        count = PLATFORM_AICORE_BUFFER_SIZE;
    }
    RecordVec *dst = &g.records[core_index];
    uint32_t skipped = 0;
    for (uint32_t i = 0; i < count; i++) {
        const L2SwimlaneAicoreTaskRecord *r = &buf->records[i];
        if (r->start_time == 0) {
            skipped++;
            continue;
        }
        recvec_push(dst, r);
    }
    g.total_aicore_collected += (uint64_t)(count - skipped);
    if (skipped > 0) {
        LOG_WARN(
            "Core %u: skipped %u AICore record slot(s) with start_time=0 (race-window write or "
            "recycled-buffer tail). buf seq=%u count=%u",
            core_index, skipped, info->buffer_seq, count
        );
    }
}

static void collector_reconcile_counters(void) {
    if (g.shm_host == NULL) {
        return;
    }
    rmb();

    int leftover_active = 0;
    for (int i = 0; i < g.num_aicore; i++) {
        L2SwimlaneAicoreTaskPool *state = get_aicore_buffer_state(g.shm_host, g.num_aicore, i);
        uint64_t buf_ptr = state->head.current_buf_ptr;
        if (buf_ptr == 0) continue;
        void *host_ptr = map_resolve((void *)buf_ptr);
        if (host_ptr == NULL) continue;
        uint32_t count = ((L2SwimlaneAicoreTaskBuffer *)host_ptr)->count;
        if (count == 0) continue;
        LOG_ERROR(
            "L2Swimlane reconcile: core %d has un-flushed AICORE buffer (current_buf_ptr=0x%lx, count=%u) "
            "after stop() — device flush failed",
            i, (unsigned long)buf_ptr, count
        );
        leftover_active++;
    }

    uint64_t total_device = 0;
    uint64_t dropped_device = 0;
    for (int i = 0; i < g.num_aicore; i++) {
        L2SwimlaneAicoreTaskPool *state = get_aicore_buffer_state(g.shm_host, g.num_aicore, i);
        total_device += state->head.total_record_count;
        dropped_device += state->head.dropped_record_count;
    }

    if (dropped_device > 0) {
        LOG_WARN(
            "L2Swimlane reconcile: %lu AICORE records dropped on device side (buffer full / ready_queue full).",
            (unsigned long)dropped_device
        );
    }
    uint64_t accounted = g.total_aicore_collected + dropped_device;
    if (accounted != total_device) {
        LOG_WARN(
            "L2Swimlane reconcile: AICORE count mismatch (collected=%lu + dropped=%lu != "
            "device_total=%lu, silent_loss=%ld)",
            (unsigned long)g.total_aicore_collected, (unsigned long)dropped_device, (unsigned long)total_device,
            (long)total_device - (long)accounted
        );
    } else {
        LOG_INFO_V0(
            "L2Swimlane reconcile: AICORE counts match (collected=%lu, dropped=%lu, device_total=%lu)",
            (unsigned long)g.total_aicore_collected, (unsigned long)dropped_device, (unsigned long)total_device
        );
    }

    if (leftover_active > 0) {
        LOG_ERROR(
            "L2Swimlane reconcile: %d core(s) had un-cleared AICORE current_buf_ptr — see prior errors",
            leftover_active
        );
    }
}

static void collector_set_core_types(const int32_t *types, int n) {
    free(g.core_types);
    g.core_types = NULL;
    g.core_types_count = 0;
    if (types == NULL || n <= 0) {
        return;
    }
    g.core_types = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    memcpy(g.core_types, types, (size_t)n * sizeof(int32_t));
    g.core_types_count = n;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

// JSON v2 emit: dump raw cycle-domain per-stream records + metadata; the join
// (AICore↔AICPU, base_time normalization, cycles→µs) lives in the python
// converter. Output must stay byte-identical to the prior C++ emitter.
static int collector_export_swimlane_json(void) {
    if (g.shm_host == NULL) {
        return -1;
    }

    bool has_any_records = false;
    for (int i = 0; i < g.num_aicore; i++) {
        if (g.records[i].size != 0) {
            has_any_records = true;
            break;
        }
    }
    if (!has_any_records) {
        LOG_WARN("Warning: No performance data to export.");
        return -1;
    }

    if (mkdir_p(g.output_prefix) != 0) {
        LOG_ERROR("Error: Failed to create output directory %s: %s", g.output_prefix, strerror(errno));
        return -1;
    }

    char filepath[PATH_MAX];
    snprintf(filepath, sizeof(filepath), "%s/l2_swimlane_records.json", g.output_prefix);
    FILE *outfile = fopen(filepath, "w");
    if (outfile == NULL) {
        LOG_ERROR("Error: Failed to open file: %s", filepath);
        return -1;
    }

    int l2_swimlane_level = (int)g.level;

    fprintf(outfile, "{\n");
    fprintf(outfile, "  \"l2_swimlane_level\": %d,\n", l2_swimlane_level);

    // metadata: clock_freq_hz drives the cycles→µs conversion (a2a3 = 50 MHz).
    fprintf(outfile, "  \"metadata\": {\n");
    fprintf(outfile, "    \"clock_freq_hz\": %llu,\n", (unsigned long long)PLATFORM_PROF_SYS_CNT_FREQ);
    fprintf(outfile, "    \"num_cores\": %d,\n", g.num_aicore);
    fprintf(outfile, "    \"core_types\": [");
    for (int i = 0; i < g.num_aicore; i++) {
        int32_t ct = (i < g.core_types_count) ? g.core_types[i] : 1 /* AIV */;
        if (i > 0) fprintf(outfile, ", ");
        fprintf(outfile, "\"%s\"", (ct == 0) ? "aic" : "aiv");
    }
    fprintf(outfile, "]\n  },\n");

    {
        fprintf(outfile, "  \"aicore_tasks\": [");
        bool first = true;
        size_t total = 0;
        for (int core_idx = 0; core_idx < g.num_aicore; core_idx++) {
            RecordVec *v = &g.records[core_idx];
            for (size_t k = 0; k < v->size; k++) {
                const L2SwimlaneAicoreTaskRecord *r = &v->data[k];
                if (!first) fprintf(outfile, ",");
                fprintf(
                    outfile, "\n    [%d, %llu, %u, %llu, %llu]", core_idx, (unsigned long long)r->task_token_raw,
                    r->reg_task_id, (unsigned long long)r->start_time, (unsigned long long)r->end_time
                );
                first = false;
                total++;
            }
        }
        if (!first) fprintf(outfile, "\n  ");
        fprintf(outfile, "]");
        LOG_INFO_V0("  aicore_tasks: %zu records", total);
    }

    fprintf(outfile, "\n}\n");

    int stream_err = ferror(outfile);
    if (fclose(outfile) != 0 || stream_err != 0) {
        LOG_ERROR("Failed to write JSON file (stream error): %s", filepath);
        return -1;
    }

    LOG_INFO_V0("=== JSON Export Complete ===");
    LOG_INFO_V0("File: %s", filepath);
    return 0;
}

static int collector_finalize(sw_unregister_fn unregister_cb, sw_free_fn free_cb) {
    if (g.shm_host == NULL) {
        return 0;
    }

    collector_stop();

    LOG_DEBUG("Cleaning up performance profiling resources");

    // Every release goes through release_one_buffer so unregister + free stay a
    // pair (each halHostRegister mapping is taken down before its device memory
    // is freed; otherwise the HAL registration table leaks across runs).
    release_one_buffer(g.aicore_ring_addr_table_dev, unregister_cb, free_cb);
    g.aicore_ring_addr_table_dev = NULL;

    release_owned_buffers(unregister_cb, free_cb);

    for (int i = 0; i < g.num_aicore; i++) {
        L2SwimlaneAicoreTaskPool *ac_state = get_aicore_buffer_state(g.shm_host, g.num_aicore, i);
        release_one_buffer((void *)ac_state->head.current_buf_ptr, unregister_cb, free_cb);
        ac_state->head.current_buf_ptr = 0;
        drain_free_queue(&ac_state->free_queue, unregister_cb, free_cb);
    }

    release_one_buffer(g.perf_shared_mem_dev, unregister_cb, free_cb);
    LOG_DEBUG("Main shm released");

    g.perf_shared_mem_dev = NULL;
    g.shm_host = NULL;

    for (int i = 0; i < g.num_aicore; i++) {
        free(g.records[i].data);
    }
    free(g.records);
    g.records = NULL;
    free(g.core_types);
    g.core_types = NULL;
    g.core_types_count = 0;
    g.total_aicore_collected = 0;

    // Drop pool containers + memory context.
    free(g.map);
    g.map = NULL;
    g.map_size = g.map_cap = 0;
    free(g.recycled);
    g.recycled = NULL;
    g.rec_size = g.rec_cap = 0;
    free(g.ready);
    g.ready = NULL;
    g.ready_cap = g.ready_head = g.ready_count = 0;
    free(g.done);
    g.done = NULL;
    g.done_cap = g.done_head = g.done_count = 0;

    pthread_mutex_destroy(&g.ready_mtx);
    pthread_cond_destroy(&g.ready_cv);
    pthread_mutex_destroy(&g.done_mtx);

    g.alloc_cb = NULL;
    g.register_cb = NULL;
    g.free_cb = NULL;
    g.shm_dev = NULL;
    g.shm_size = 0;
    g.device_id = -1;

    LOG_DEBUG("Performance profiling cleanup complete");
    return 0;
}

// =============================================================================
// HAL / ACL plumbing
// =============================================================================

static void *g_hal_handle = NULL;

typedef int (*HalHostRegisterFn)(void *dev_ptr, size_t size, unsigned int flags, int device_id, void **host_ptr);
typedef int (*HalHostUnregisterFn)(void *host_ptr, int device_id);

static int load_hal_if_needed(void) {
    if (g_hal_handle != NULL) {
        return 0;
    }
    g_hal_handle = dlopen("libascend_hal.so", RTLD_NOW | RTLD_LOCAL);
    return g_hal_handle != NULL ? 0 : -1;
}

static HalHostRegisterFn get_hal_host_register(void) {
    if (g_hal_handle == NULL) {
        return NULL;
    }
    return (HalHostRegisterFn)dlsym(g_hal_handle, "halHostRegister");
}

static HalHostUnregisterFn get_hal_host_unregister(void) {
    if (g_hal_handle == NULL) {
        return NULL;
    }
    return (HalHostUnregisterFn)dlsym(g_hal_handle, "halHostUnregister");
}

static void *acl_alloc(size_t size) {
    void *ptr = NULL;
    if (aclrtMalloc(&ptr, size, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS) {
        return NULL;
    }
    return ptr;
}

static int acl_free(void *ptr) {
    return ptr != NULL ? (int)aclrtFree(ptr) : 0;
}

static int hal_register(void *dev_ptr, size_t size, int device_id, void **host_ptr) {
    if (load_hal_if_needed() != 0) {
        return -1;
    }
    HalHostRegisterFn fn = get_hal_host_register();
    if (fn == NULL) {
        return -1;
    }
    return fn(dev_ptr, size, DEV_SVM_MAP_HOST, device_id, host_ptr);
}

static int hal_unregister(void *host_ptr, int device_id) {
    if (load_hal_if_needed() != 0) {
        return -1;
    }
    HalHostUnregisterFn fn = get_hal_host_unregister();
    if (fn == NULL) {
        return -1;
    }
    return fn(host_ptr, device_id);
}

// =============================================================================
// Module-level state + C ABI
// =============================================================================

static L2SwimlaneLevel s_level = L2_SWIMLANE_LEVEL_AICORE_TIMING;
static bool s_initialized = false;
static char s_output_prefix[PATH_MAX] = ".";

static void export_perfetto_trace_json(void) {
    const char *esl_root = getenv("ESL_PROXY_ROOT");
    if (esl_root == NULL || esl_root[0] == '\0') {
        esl_root = ".";
    }
    char raw[PATH_MAX];
    char out[PATH_MAX];
    char script[PATH_MAX];
    char cmd[PATH_MAX * 4];
    snprintf(raw, sizeof(raw), "%s/l2_swimlane_records.json", s_output_prefix);
    snprintf(out, sizeof(out), "%s/l2_swimlane_trace.json", s_output_prefix);
    snprintf(script, sizeof(script), "%s/tools/swimlane_to_perfetto.py", esl_root);
    snprintf(cmd, sizeof(cmd), "python3 '%s' '%s' -o '%s' >/dev/null 2>&1", script, raw, out);

    if (system(cmd) == 0) {
        fprintf(stderr, "[esl_proxy] Perfetto trace: %s\n", out);
        fprintf(stderr, "[esl_proxy] Import at https://ui.perfetto.dev/\n");
    } else {
        fprintf(
            stderr,
            "[esl_proxy] Perfetto conversion failed; raw swimlane data at %s (run: python3 %s %s)\n", raw, script, raw
        );
    }
}

void esl_swimlane_host_set_level(int level) {
    if (level < 0) {
        s_level = L2_SWIMLANE_LEVEL_DISABLED;
        return;
    }
    if (level > (int)L2_SWIMLANE_LEVEL_AICORE_TIMING) {
        level = (int)L2_SWIMLANE_LEVEL_AICORE_TIMING;
    }
    s_level = (L2SwimlaneLevel)level;
}

int esl_swimlane_host_init(int worker_count, int aicpu_thread_num, int device_id, const char *output_prefix) {
    if (s_level == L2_SWIMLANE_LEVEL_DISABLED || worker_count <= 0) {
        return 0;
    }
    snprintf(s_output_prefix, sizeof(s_output_prefix), "%s",
             (output_prefix != NULL && output_prefix[0] != '\0') ? output_prefix : ".");

    int rc = collector_initialize(
        worker_count, aicpu_thread_num, device_id, s_level, acl_alloc, hal_register, acl_free, s_output_prefix
    );
    if (rc != 0) {
        fprintf(stderr, "[esl_proxy] swimlane init failed rc=%d\n", rc);
        return rc;
    }
    s_initialized = true;
    return 0;
}

void esl_swimlane_host_start(void) {
    if (!s_initialized) {
        return;
    }
    // CANN device context is per-thread; the mgmt/poll threads call aclrtMalloc
    // when recycling buffers, so each thread entry sets the device first.
    collector_start();
}

void esl_swimlane_host_stop_and_export(void) {
    if (!s_initialized) {
        return;
    }
    collector_stop();
    collector_reconcile_counters();
    if (collector_export_swimlane_json() == 0) {
        export_perfetto_trace_json();
    }
}

void esl_swimlane_host_finalize(void) {
    if (!s_initialized) {
        return;
    }
    collector_finalize(hal_unregister, acl_free);
    s_initialized = false;
}

uint64_t esl_swimlane_host_data_base(void) {
    if (!s_initialized) {
        return 0;
    }
    return (uint64_t)(uintptr_t)g.perf_shared_mem_dev;
}

uint64_t esl_swimlane_host_rotation_table(void) {
    if (!s_initialized) {
        return 0;
    }
    return (uint64_t)(uintptr_t)g.aicore_ring_addr_table_dev;
}

void esl_swimlane_host_set_core_types(const int32_t *core_types, int count) {
    if (!s_initialized || core_types == NULL || count <= 0) {
        return;
    }
    collector_set_core_types(core_types, count);
}

static int parse_swimlane_level_from_env(void) {
    const char *env_val = getenv("ESL_PROXY_L2_SWIMLANE_LEVEL");
    int level = 0;

    if (env_val != NULL && env_val[0] != '\0') {
        level = atoi(env_val);
        if (level < 0) {
            level = 0;
        } else if (level > 1) {
            level = 1;
        }
    }
    return level;
}

int esl_swimlane_host_onboard_begin(int device_id, const char *output_prefix) {
    const int level = parse_swimlane_level_from_env();

    esl_swimlane_host_set_level(level);
    if (level > 0) {
        fprintf(stderr, "[esl_proxy] L2 swimlane enabled level=%d (no PMU)\n", level);
        return esl_swimlane_host_init(ESL_PROXY_ONBOARD_WORKER_COUNT, ESL_PROXY_AICPU_THREAD_NUM, device_id,
                                      output_prefix);
    }
    return 0;
}

void esl_swimlane_host_onboard_fill_kargs(void *k_args_opaque) {
    EslKernelArgs *k_args = (EslKernelArgs *)k_args_opaque;
    if (k_args == NULL || s_level == L2_SWIMLANE_LEVEL_DISABLED) {
        return;
    }
    k_args->l2_swimlane_data_base = esl_swimlane_host_data_base();
    k_args->l2_swimlane_aicore_rotation_table = esl_swimlane_host_rotation_table();
    ESL_SWIMLANE_PROFILING_FLAG_ON(k_args->enable_profiling_flag);
}

void esl_swimlane_host_onboard_sync_core_types(void *dev_runtime_dev_ptr) {
    if (s_level == L2_SWIMLANE_LEVEL_DISABLED || dev_runtime_dev_ptr == NULL) {
        return;
    }
    EslRuntime dev_runtime_host;
    int32_t core_types[ESL_PROXY_ONBOARD_WORKER_COUNT];
    int w;

    memset(&dev_runtime_host, 0, sizeof(dev_runtime_host));
    if (aclrtMemcpy(&dev_runtime_host, sizeof(dev_runtime_host), dev_runtime_dev_ptr, sizeof(EslRuntime),
                    ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
        return;
    }
    for (w = 0; w < ESL_PROXY_ONBOARD_WORKER_COUNT; ++w) {
        core_types[w] = dev_runtime_host.workers[w].core_type;
    }
    esl_swimlane_host_set_core_types(core_types, ESL_PROXY_ONBOARD_WORKER_COUNT);
}

void esl_swimlane_host_onboard_end(void) {
    esl_swimlane_host_stop_and_export();
    esl_swimlane_host_finalize();
}
