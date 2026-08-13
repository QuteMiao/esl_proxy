/*
 * early_dispatch.c - Early-dispatch 一期基础设施实现（Step 1）
 *
 * 本文件提供：ed 全局变量定义、ed_init、generation-tagged record helper、
 * per-pred 边锁、ring 换代元数据重置（ed_init_task_meta）。
 * Hook 0/1/2 在 Step 4–7 逐步接入；此处仅 stub 保证链接通过。
 */

#include "early_dispatch.h"

#include <assert.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include "dispatch.h"
#include "executor.h"
#include "lat_trace.h"
#include "log.h"
#include "ring_buf.h"
#include "spin.h"
#include "swimlane.h"
#include "task.h"

#ifndef ED_A10_FORCE_SELF_NOTIFY
#define ED_A10_FORCE_SELF_NOTIFY 0
#endif

/* cutter.c 中分配；ed_init_task_meta 需清 successor_cnt */
extern task_state *g_state_buf;

/* -------------------------------------------------------------------------
 * 全局变量定义
 * ------------------------------------------------------------------------- */
_Atomic uint16_t  g_dispatch_fanin[RING_SIZE];
uint16_t          g_dispatch_fanin_target[RING_SIZE];
_Atomic uint16_t  g_unfin_pred_cnt[RING_SIZE];
_Atomic uint8_t   g_spec_state[RING_SIZE];
_Atomic uint64_t  g_staged_slot_record[RING_SIZE];
_Atomic uint8_t   g_notify_claimed[RING_SIZE];
_Atomic uint16_t  g_next_block_idx[RING_SIZE];
_Atomic uint64_t  g_task_dispatch_record[RING_SIZE];
_Atomic uint32_t  g_ring_task_tag[RING_SIZE];
_Atomic uint32_t  g_dispatch_tag[RING_SIZE];
atomic_flag       g_ed_edge_lock[RING_SIZE];
ed_pred_snapshot_t g_ed_pred_snapshot[RING_SIZE];
queue_t           g_ed_ready_queue;
_Atomic uint64_t  g_ed_gated_core_mask[EXE_TYPE_CNT];

_Atomic uint64_t g_ed_stage_cnt;
_Atomic uint64_t g_ed_stage_spmd_cnt;
_Atomic uint64_t g_ed_hit_cnt;
_Atomic uint64_t g_ed_self_notify_cnt;
_Atomic uint64_t g_ed_slot_retry_cnt;
_Atomic uint64_t g_ed_block_cas_fail_cnt;
_Atomic uint64_t g_ed_send_skip_cnt;
_Atomic uint64_t g_ed_late_arrival_cnt;

#if ED_HOOK0_CONTRIB_STATS
_Atomic uint64_t g_ed_hook0_contrib_cnt;
#endif

_Atomic uint64_t g_ed_gate_open_cnt;

_Atomic uint64_t g_ed_ready_ns[RING_SIZE];
_Atomic uint32_t g_ed_ready_tag[RING_SIZE];
_Atomic uint64_t g_ed_lat_cnt[ED_LAT_PATH_CNT];
_Atomic uint64_t g_ed_lat_sum_ns[ED_LAT_PATH_CNT];
_Atomic uint64_t g_ed_lat_max_ns[ED_LAT_PATH_CNT];
_Atomic uint64_t g_ed_lat_hist[ED_LAT_PATH_CNT][ED_LAT_BUCKET_CNT];

/* -------------------------------------------------------------------------
 * 边锁：Hook 0 与 add_successors 第二趟共用，持锁时间极短
 * ------------------------------------------------------------------------- */
void ed_edge_lock(uint16_t task_idx)
{
    while (atomic_flag_test_and_set_explicit(&g_ed_edge_lock[task_idx],
                                             memory_order_acquire)) {
        spin_wait();
    }
}

void ed_edge_unlock(uint16_t task_idx)
{
    atomic_flag_clear_explicit(&g_ed_edge_lock[task_idx], memory_order_release);
}

/* -------------------------------------------------------------------------
 * generation tag 校验
 * ------------------------------------------------------------------------- */
bool ed_record_tag_matches(uint64_t record, uint32_t task_id)
{
    return ED_RECORD_TAG(record) == task_id;
}

/* -------------------------------------------------------------------------
 * g_task_dispatch_record：瞬时 dispatch 位置（完成时清；tag 不匹配则跳过）
 * ------------------------------------------------------------------------- */
uint64_t ed_task_dispatch_record_load(uint32_t task_id)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    return atomic_load_explicit(&g_task_dispatch_record[idx], memory_order_acquire);
}

void ed_task_dispatch_record_store(uint32_t task_id, int core, int slot, int type)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    uint32_t packed = ED_PACK_SLOT((uint16_t)core, (uint8_t)slot, (uint8_t)type);
    uint64_t record = ED_PACK_RECORD(task_id, packed);

    atomic_store_explicit(&g_task_dispatch_record[idx], record, memory_order_release);
}

void ed_task_dispatch_record_clear(uint32_t task_id)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    uint64_t expected = atomic_load_explicit(&g_task_dispatch_record[idx],
                                            memory_order_acquire);

    if (!ed_record_tag_matches(expected, task_id)) {
        /* ring slot 已被下一代覆写，或 record 从未写入：拒绝清除，防误清新代 */
        WORKER_LOGF("stale_tag, dispatch_record_clear skipped, task=%u, record_tag=%u",
                    task_id, ED_RECORD_TAG(expected));
        return;
    }

    /*
     * CAS 清除：load 与 store 之间若同 slot 被新代覆写，CAS 失败并跳过，
     * 避免 TOCTOU 误清新代瞬时位置（rev Round 1 必修项）。
     */
    uint64_t invalid = ED_RECORD_INVALID;
    if (!atomic_compare_exchange_strong_explicit(
            &g_task_dispatch_record[idx], &expected, invalid,
            memory_order_acq_rel, memory_order_acquire)) {
        WORKER_LOGF("stale_tag, dispatch_record_clear cas failed, task=%u, saw_tag=%u",
                    task_id, ED_RECORD_TAG(expected));
    }
}

/* -------------------------------------------------------------------------
 * ed_init_task_meta：ring slot 换代时一次性重置全部代际字段（T0 事件，§5.20）
 * ------------------------------------------------------------------------- */
void ed_init_task_meta(uint32_t full_task_id, uint16_t predecessor_cnt)
{
#if ED_ENABLE
    uint16_t task_idx = (uint16_t)(full_task_id & RING_MASK);

    /*
     * 锁内清 outgoing edge 与 ring_task_tag，与 Hook 0 / add_successors 第二趟
     * 共用同一把 s 的边锁，防止 append 与换代 tearing。
     */
    ed_edge_lock(task_idx);
    atomic_store_explicit(&g_ring_task_tag[task_idx], full_task_id,
                          memory_order_release);
    atomic_store_explicit(&g_dispatch_tag[task_idx], ED_TASK_TAG_INVALID,
                          memory_order_relaxed);
    g_successor_buf[task_idx].cnt = 0;
    if (g_state_buf != NULL) {
        g_state_buf[task_idx].successor_cnt = 0;
    }
    ed_edge_unlock(task_idx);

    g_dispatch_fanin_target[task_idx] = predecessor_cnt;
    atomic_store_explicit(&g_dispatch_fanin[task_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_unfin_pred_cnt[task_idx], predecessor_cnt,
                          memory_order_relaxed);
    atomic_store_explicit(&g_spec_state[task_idx], ED_SPEC_NONE,
                          memory_order_relaxed);
    atomic_store_explicit(&g_next_block_idx[task_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_staged_slot_record[task_idx], ED_RECORD_INVALID,
                          memory_order_relaxed);
    atomic_store_explicit(&g_task_dispatch_record[task_idx], ED_RECORD_INVALID,
                          memory_order_relaxed);
    atomic_store_explicit(&g_notify_claimed[task_idx], 0, memory_order_relaxed);
    g_ed_pred_snapshot[task_idx].cnt = 0;
#else
    (void)full_task_id;
    (void)predecessor_cnt;
#endif
}

/* -------------------------------------------------------------------------
 * ed_init：进程启动时调用，所有 ring 表项置 INVALID
 * ------------------------------------------------------------------------- */
void ed_init(void)
{
    for (int i = 0; i < RING_SIZE; i++) {
        atomic_init(&g_dispatch_fanin[i], 0);
        g_dispatch_fanin_target[i] = 0;
        atomic_init(&g_unfin_pred_cnt[i], 0);
        atomic_init(&g_spec_state[i], ED_SPEC_NONE);
        atomic_init(&g_staged_slot_record[i], ED_RECORD_INVALID);
        atomic_init(&g_notify_claimed[i], 0);
        atomic_init(&g_next_block_idx[i], 0);
        atomic_init(&g_task_dispatch_record[i], ED_RECORD_INVALID);
        atomic_init(&g_ring_task_tag[i], ED_TASK_TAG_INVALID);
        atomic_init(&g_dispatch_tag[i], ED_TASK_TAG_INVALID);
        atomic_flag_clear(&g_ed_edge_lock[i]);
        g_ed_pred_snapshot[i].cnt = 0;
        atomic_init(&g_ed_ready_ns[i], 0);
        atomic_init(&g_ed_ready_tag[i], ED_TASK_TAG_INVALID);
    }

    memset(&g_ed_ready_queue, 0, sizeof g_ed_ready_queue);
    atomic_flag_clear(&g_ed_ready_queue.lock);

    for (int t = 0; t < EXE_TYPE_CNT; t++) {
        atomic_init(&g_ed_gated_core_mask[t], 0);
    }

    atomic_store_explicit(&g_ed_stage_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_stage_spmd_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_hit_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_self_notify_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_slot_retry_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_block_cas_fail_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_send_skip_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_late_arrival_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ed_gate_open_cnt, 0, memory_order_relaxed);
#if ED_HOOK0_CONTRIB_STATS
    atomic_store_explicit(&g_ed_hook0_contrib_cnt, 0, memory_order_relaxed);
#endif

    for (int k = 0; k < ED_LAT_PATH_CNT; k++) {
        atomic_store_explicit(&g_ed_lat_cnt[k], 0, memory_order_relaxed);
        atomic_store_explicit(&g_ed_lat_sum_ns[k], 0, memory_order_relaxed);
        atomic_store_explicit(&g_ed_lat_max_ns[k], 0, memory_order_relaxed);
        for (int b = 0; b < ED_LAT_BUCKET_CNT; b++) {
            atomic_store_explicit(&g_ed_lat_hist[k][b], 0, memory_order_relaxed);
        }
    }
}

/* -------------------------------------------------------------------------
 * ready->runnable 延迟采样
 * ------------------------------------------------------------------------- */
void ed_lat_mark_ready(uint32_t task_id)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    /*
     * 先写时间戳（relaxed），再 release 发布 tag：
     * 读侧 acquire 读到本代 tag 后，才保证能看到配套的时间戳。
     */
    atomic_store_explicit(&g_ed_ready_ns[idx], get_time_ns_hires(), memory_order_relaxed);
    atomic_store_explicit(&g_ed_ready_tag[idx], (uint32_t)task_id, memory_order_release);
}

void ed_lat_mark_runnable(uint32_t task_id, int path)
{
    uint16_t idx = (uint16_t)(task_id & RING_MASK);
    uint32_t expected_tag = (uint32_t)task_id;
    /*
     * tag 不匹配的两种正常情况，都应当丢弃样本：
     * 1) 零前驱任务从不打 ready 点，不存在"空等依赖"这段延迟；
     * 2) 样本已被本代首次调用消费。
     * 用 CAS 把 tag 置回 INVALID，保证每代任务只贡献一个样本。
     *
     * 注意 count>1 的 SPMD 任务同样只派发一次（整任务一个条目、全部 block 在同一
     * 槽位顺序执行），因此这里不存在"同一代被多次采样"的情形。
     */
    if (!atomic_compare_exchange_strong_explicit(
            &g_ed_ready_tag[idx], &expected_tag, ED_TASK_TAG_INVALID,
            memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

    uint64_t t_ready = atomic_load_explicit(&g_ed_ready_ns[idx], memory_order_relaxed);
    uint64_t t_run = get_time_ns_hires();
    if (t_run < t_ready) {
        return;
    }
    uint64_t lat = t_run - t_ready;
    int k = (path == ED_LAT_EARLY) ? ED_LAT_EARLY : ED_LAT_NORMAL;

    atomic_fetch_add_explicit(&g_ed_lat_cnt[k], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_ed_lat_sum_ns[k], lat, memory_order_relaxed);

    uint64_t prev = atomic_load_explicit(&g_ed_lat_max_ns[k], memory_order_relaxed);
    while (lat > prev &&
           !atomic_compare_exchange_weak_explicit(&g_ed_lat_max_ns[k], &prev, lat,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
        /* CAS 失败时 prev 已被刷成最新值，直接重试 */
    }

    /* 桶号 = lat 的二进制位宽，即 lat 落在 [2^(b-1), 2^b) */
    int b = (lat == 0) ? 0 : (64 - __builtin_clzll(lat));
    if (b >= ED_LAT_BUCKET_CNT) {
        b = ED_LAT_BUCKET_CNT - 1;
    }
    atomic_fetch_add_explicit(&g_ed_lat_hist[k][b], 1, memory_order_relaxed);
}

/* Step 6 接入；Step 1 空实现 */
void ed_notify_once(uint32_t task_id, uint64_t record, ed_notify_source_t source)
{
#if ED_ENABLE
    uint16_t s_idx = (uint16_t)(task_id & RING_MASK);
    /*
     * Step 7 ABA 防护：
     * 1) ring_task_tag 必须仍指向本代 task_id（拒绝旧代迟到通知）；
     * 2) staged_slot_record 必须与调用方 record 完全一致（拒绝旧位置误放行）。
     */
    uint32_t live_tag = atomic_load_explicit(&g_ring_task_tag[s_idx], memory_order_acquire);
    if (live_tag != task_id) {
        return;
    }
    uint64_t live_record =
        atomic_load_explicit(&g_staged_slot_record[s_idx], memory_order_seq_cst);
    if (live_record != record) {
        return;
    }
    if (!ed_record_tag_matches(record, task_id)) {
        return;
    }

    uint32_t packed = ED_RECORD_SLOT(record);
    uint16_t core = ED_UNPACK_CORE(packed);
    uint8_t slot = ED_UNPACK_SLOT(packed);
    uint8_t type = ED_UNPACK_TYPE(packed);
    if (core >= AIC_CNT || slot >= AIC_OSTD || type >= EXE_TYPE_CNT) {
        return;
    }

    uint8_t expected_claim = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_notify_claimed[s_idx], &expected_claim, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

    /*
     * notify 只负责敲门铃，不改 slot_state：
     * GATED->RUNNABLE 的翻转、门铃清零、KPI 收口统一由 executor 完成，
     * 对应硬件语义「AI Core 轮询到 doorbell 置位后自行启动」。
     *
     * 去掉原先这里的 slot_state CAS 不削弱 ABA 防护：上面已用 g_ring_task_tag
     * 与 g_staged_slot_record 做过换代校验，g_notify_claimed 保证唯一写者，
     * 而 executor 侧 CAS(expect GATED) 是最后一道「槽位仍在门禁态」的把关。
     */
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 1, memory_order_release);
    WORKER_LOGF("notify_write, s=%u, source=%s",
                task_id, (source == ED_NOTIFY_HOOK2) ? "hook2" : "hook1");
    swim_mark(task_id, (source == ED_NOTIFY_HOOK2) ? SWIM_MK_NOTIFY_H2
                                                   : SWIM_MK_NOTIFY_H1);
    if (source == ED_NOTIFY_HOOK2) {
        atomic_fetch_add_explicit(&g_ed_hit_cnt, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&g_ed_self_notify_cnt, 1, memory_order_relaxed);
    }
#else
    (void)task_id;
    (void)record;
    (void)source;
#endif
}

#if ED_ENABLE
static __thread unsigned int s_ed_rand_seed;

static inline void ed_enqueue_or_abandon(uint32_t task_id)
{
    if (enqueue(&g_ed_ready_queue, task_id)) {
        return;
    }

    uint16_t s_idx = (uint16_t)(task_id & RING_MASK);
    uint8_t expected = ED_SPEC_STAGING;
    (void)atomic_compare_exchange_strong_explicit(
        &g_spec_state[s_idx], &expected, ED_SPEC_NONE,
        memory_order_acq_rel, memory_order_acquire);
}

static inline void ed_maybe_enter_staging(uint32_t s_id, uint16_t s_idx, uint16_t fanin_now)
{
    if (fanin_now != g_dispatch_fanin_target[s_idx]) {
        return;
    }
    /*
     * count>1（SPMD）现在也准入：整任务作为一个条目 stage 到单个槽位，executor
     * 照旧在该槽位上连续跑完全部 block，完成语义不变。上限由 ED_SPMD_MAX_BLOCKS
     * 控制，count==0 的 phantom（尚未 new_task 的脏槽）恒被拒。
     */
    if (!ed_count_admitted(g_basic_buf[s_idx].count)) {
        return;
    }

    uint16_t unfin = atomic_load_explicit(&g_unfin_pred_cnt[s_idx], memory_order_acquire);
    if (unfin > ED_UNFIN_THRESHOLD) {
        return;
    }

    uint8_t expected = ED_SPEC_NONE;
    if (atomic_compare_exchange_strong_explicit(
            &g_spec_state[s_idx], &expected, ED_SPEC_STAGING,
            memory_order_acq_rel, memory_order_relaxed)) {
        ed_enqueue_or_abandon(s_id);
    }
}

void propagate_dispatch_fanin(uint32_t p_id)
{
    uint16_t p_idx = (uint16_t)(p_id & RING_MASK);
    ed_edge_lock(p_idx);
    if (atomic_load_explicit(&g_ring_task_tag[p_idx], memory_order_acquire) != (uint32_t)p_id) {
        ed_edge_unlock(p_idx);
        return;
    }

    /* 持久记录“该 generation 曾 dispatch”；完成时不清。 */
    atomic_store_explicit(&g_dispatch_tag[p_idx], (uint32_t)p_id, memory_order_release);

    uint32_t succ_cnt = g_successor_buf[p_idx].cnt;
    for (uint32_t k = 0; k < succ_cnt; k++) {
        uint32_t s_id = g_successor_buf[p_idx].node[k];
        uint16_t s_idx = (uint16_t)(s_id & RING_MASK);

        /*
         * fanin 记账对所有 count 一律无条件累加，count 过滤只发生在
         * ed_maybe_enter_staging 的准入判断里。
         *
         * 这里曾经有一道 count != 1 的 continue，它是 count>1 任务永远进不了 ED 的
         * 真正原因：staging 的第一道条件是 fanin == fanin_target，计数不涨则条件
         * 永不满足。另一个必须无条件的理由是对称性——cutter 的两条 late-arrival
         * 路径本来就对所有 count 累加 fanin，若此处按 count 过滤，同一个任务的
         * fanin 会由两个口径不一致的生产者共同写入，target 可能永远达不到。
         */
        uint16_t fanin_now = (uint16_t)(atomic_fetch_add_explicit(
                                 &g_dispatch_fanin[s_idx], 1, memory_order_relaxed) +
                             1);
#if ED_HOOK0_CONTRIB_STATS
        atomic_fetch_add_explicit(&g_ed_hook0_contrib_cnt, 1, memory_order_relaxed);
#endif
        ed_maybe_enter_staging(s_id, s_idx, fanin_now);
    }

    ed_edge_unlock(p_idx);
}

static int pick_stage_core(int tid, uint32_t s_id, task_type_t type, int *out_slot)
{
    uint16_t s_idx = (uint16_t)(s_id & RING_MASK);
    uint64_t pcore_bitmap = 0;
    ed_pred_snapshot_t *snap = &g_ed_pred_snapshot[s_idx];
    for (uint16_t k = 0; k < snap->cnt; k++) {
        uint32_t p_id = snap->node[k];
        uint16_t p_idx = (uint16_t)(p_id & RING_MASK);
        if (g_state_buf != NULL &&
            g_state_buf[p_idx].state == TASK_STATUS_COMPLETED) {
            continue;
        }

        uint64_t record = ed_task_dispatch_record_load(p_id);
        if (!ed_record_tag_matches(record, p_id)) {
            continue;
        }

        uint32_t packed = ED_RECORD_SLOT(record);
        if ((task_type_t)ED_UNPACK_TYPE(packed) != type) {
            continue;
        }
        uint16_t core = ED_UNPACK_CORE(packed);
        if (core >= AIC_CNT) {
            continue;
        }
        pcore_bitmap |= ((uint64_t)1u << core);
    }

    uint64_t free_slot[AIC_OSTD];
    uint64_t free_any = 0;               /* 本 type 下任一 slot 空闲的核 */
    uint64_t free_all = ~(uint64_t)0;    /* 本 type 下全部 slot 空闲的核 */
    for (int j = 0; j < AIC_OSTD; j++) {
        free_slot[j] = atomic_load_explicit(&g_ctrl_t[tid].free_bitmap[type][j],
                                            memory_order_acquire);
        free_any |= free_slot[j];
        free_all &= free_slot[j];
    }

    /*
     * 一选：前驱所在核，本 type 下有任一 slot 空闲即可（数据局部性优先）。
     * 注意 pcore_bitmap 只收同 type 的前驱（见上面的 ED_UNPACK_TYPE 校验），
     * 因此算子流水呈 CUBE/VECTOR 交替时（如 paged_attention 的
     * qk_matmul->softmax_prep->pv_matmul->online_update），每条边两端 type
     * 必然不同，这一选恒为空、每次都落到二选。
     */
    uint64_t candidate = pcore_bitmap & free_any;
    if (candidate == 0) {
        /*
         * 二选：本 type 下全部 slot 都空闲的核，不再限定前驱所在核。
         * 口径是 type 粒度，不是「整个物理核空闲」——free_bitmap 的第一维就是
         * type，同一核编号在不同 type 下是各自独立的槽位组。
         *
         * 不退化到「忙核的空余 slot」——staged 任务得等该核当前任务跑完才能
         * 开跑，ED 的提前收益归零，还白占一个预装载名额不让 normal 用。
         * 这种情况宁可 re-push 重试（见 g_ed_slot_retry_cnt）。
         */
        candidate = free_all;
    }
    if (candidate == 0) {
        return -1;
    }

    if (s_ed_rand_seed == 0) {
        s_ed_rand_seed = ((unsigned int)tid + 1u) ^ ((unsigned int)s_id << 8);
        if (s_ed_rand_seed == 0) {
            s_ed_rand_seed = 1u;
        }
    }
    int popcnt = __builtin_popcountll(candidate);
    int nth = (int)(rand_r(&s_ed_rand_seed) % (unsigned int)popcnt);
    int core = pick_nth_bit(candidate, nth);
    uint64_t mask = (uint64_t)1u << (uint64_t)core;
    /* 取编号最小的空闲 slot；candidate 由 free_slot[] 推导，必有一个命中。 */
    *out_slot = 0;
    for (int j = 0; j < AIC_OSTD; j++) {
        if ((free_slot[j] & mask) != 0) {
            *out_slot = j;
            break;
        }
    }
    return core;
}

int try_early_dispatch(int tid)
{
    uint32_t s_id = 0;
    if (!dequeue(&g_ed_ready_queue, &s_id)) {
        return 0;
    }

    uint16_t s_idx = (uint16_t)(s_id & RING_MASK);
    if (atomic_load_explicit(&g_spec_state[s_idx], memory_order_seq_cst) !=
        ED_SPEC_STAGING) {
        return 0;
    }

    /*
     * 换代校验必须早于读 count：count 是非原子 payload，且下面要当作 CAS 的目标值。
     * 若读到上一代的 count，认领量就与 send_task 的 0->count 不一致，收尾自检
     * （main.c 的 block_leaked）会误报，且两条路径的互斥前提被破坏。
     * tag 不匹配说明该 ring slot 已换代，本代 spec_state 已被 ed_init_task_meta
     * 重置，直接丢弃即可，不能 re-push。
     */
    if (atomic_load_explicit(&g_ring_task_tag[s_idx], memory_order_acquire) !=
        (uint32_t)s_id) {
        return 0;
    }

    /*
     * 准入必须在 CAS 之前，且 count==0 尤其不能漏：CAS(nbi, expect 0, desire 0) 会
     * 平凡成功并返回 true，ED 会误以为认领成功而把 phantom 槽位发布进 executor。
     * 与 send_task 的 R18 兜底对齐。
     *
     * 这里重复判一次（两个 staging 入口已判过）不是冗余：s_id 是从队列里取出来的，
     * 入队与出队之间 ring slot 可能换代，count 已经不是入队时那个了。
     */
    uint16_t count = g_basic_buf[s_idx].count;
    if (!ed_count_admitted(count)) {
        return 0;
    }

    task_type_t type = g_basic_buf[s_idx].type;
    int slot = -1;
    int core = pick_stage_core(tid, s_id, type, &slot);
    if (core < 0) {
        goto re_push_slot_busy;
    }

    uint64_t core_mask = (uint64_t)1u << (uint64_t)core;
    uint64_t old_bm = atomic_fetch_and_explicit(
        &g_ctrl_t[tid].free_bitmap[type][slot], ~core_mask, memory_order_acq_rel);
    if ((old_bm & core_mask) == 0) {
        goto re_push_slot_busy;
    }

    /*
     * 整任务原子认领 0 -> count，与 send_task 完全同口径。
     * 不能写字面量 1：main.c 收尾自检以 nbi == count 判定 block_leaked，对 count>1
     * 的任务写 1 会误报泄漏。count==1 时本式退化为原来的 0->1。
     */
    uint16_t expected_blk = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_next_block_idx[s_idx], &expected_blk, count, memory_order_acq_rel,
            memory_order_relaxed)) {
        atomic_fetch_or_explicit(&g_ctrl_t[tid].free_bitmap[type][slot], core_mask,
                                 memory_order_release);
        atomic_fetch_add_explicit(&g_ed_block_cas_fail_cnt, 1, memory_order_relaxed);
        return 0;
    }

    assert(atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                memory_order_acquire) == EXE_SLOT_EMPTY);
    /*
     * 必须在发布 GATED 之前清门铃：吸收上一代残留通知，否则 executor 可能
     * 拿旧门铃把本代任务提前放行。用 relaxed 足够——下面发布 GATED 的
     * release 存储会保证本次清零对任何 acquire 侧可见。
     */
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 0,
                          memory_order_relaxed);
    g_executors[type][core].tasks[slot] = s_id;
    g_executors[type][core].block_idx[slot] = 0;
    uint32_t raw_duration = g_basic_buf[s_idx].duration;
    g_executors[type][core].duration[slot] = SCALE_EXEC_DURATION(raw_duration);
    g_ctrl_t[tid].task_id_map[slot][type][core] = s_id;
    /* 泳道 GATED 段起点；必须早于发布 GATED，理由同 send_task 里那处 */
    swim_task_stage(s_id, core, (int)type, slot, count);
    atomic_store_explicit(&g_executors[type][core].slot_state[slot],
                          EXE_SLOT_GATED, memory_order_release);
    /*
     * 置位必须晚于上面发布 GATED，且必须是 seq_cst：executor 侧是「先清位、
     * 再读 slot_state」，两边顺序相反才能保证不漏。若这里用 relaxed，
     * executor 可能先清掉位、又没看到 GATED，该槽位就永远没人来开闸。
     */
    atomic_fetch_or_explicit(&g_ed_gated_core_mask[type], core_mask,
                             memory_order_seq_cst);

#if ED_A10_FORCE_SELF_NOTIFY
    /*
     * A10 probe: 放大 "Hook2 先置 DISPATCHED、Hook1 后补 notify" 的竞争窗口。
     * 仅测试宏开启时生效，不进入常规路径。
     */
    for (int spin = 0; spin < 200000; spin++) {
        if (atomic_load_explicit(&g_spec_state[s_idx], memory_order_acquire) ==
            ED_SPEC_DISPATCHED) {
            break;
        }
        sched_yield();
    }
#endif

    uint32_t packed = ED_PACK_SLOT((uint16_t)core, (uint8_t)slot, (uint8_t)type);
    uint64_t record = ED_PACK_RECORD((uint32_t)s_id, packed);
    atomic_store_explicit(&g_staged_slot_record[s_idx], record, memory_order_seq_cst);
    atomic_fetch_add_explicit(&g_ed_stage_cnt, 1, memory_order_relaxed);
    /*
     * 按 count 分档，用于归因：ED 的收益是「一次 ready->runnable 空等」的固定节省，
     * 而 count=N 的任务执行时长是 N 倍，单任务相对收益约按 1/N 衰减。因此总覆盖率
     * 上涨多少来自 SPMD 任务必须能单独看到，否则无法判断收益是被摊薄还是真提升。
     */
    if (count > 1) {
        atomic_fetch_add_explicit(&g_ed_stage_spmd_cnt, 1, memory_order_relaxed);
    }

#if ED_A10_FORCE_SELF_NOTIFY
    /*
     * A10 probe only: 强制走 Hook1 自敲分支，专门验证 notify_once 单写语义。
     * 该宏仅测试脚本注入，不进入常规构建。
     */
    atomic_store_explicit(&g_spec_state[s_idx], ED_SPEC_DISPATCHED, memory_order_release);
#endif

    if (atomic_load_explicit(&g_spec_state[s_idx], memory_order_seq_cst) ==
        ED_SPEC_DISPATCHED) {
        ed_notify_once(s_id, record, ED_NOTIFY_HOOK1);
    }
    return 1;

re_push_slot_busy:
    if (!enqueue(&g_ed_ready_queue, s_id)) {
        uint8_t expected = ED_SPEC_STAGING;
        (void)atomic_compare_exchange_strong_explicit(
            &g_spec_state[s_idx], &expected, ED_SPEC_NONE, memory_order_acq_rel,
            memory_order_acquire);
    }
    atomic_fetch_add_explicit(&g_ed_slot_retry_cnt, 1, memory_order_relaxed);
    return 0;
}
#endif

/* -------------------------------------------------------------------------
 * 逐任务延迟分解探针（LAT_TRACE，诊断用）
 * ------------------------------------------------------------------------- */
#if LAT_TRACE
_Atomic uint64_t g_lt_ready_ns[RING_SIZE];
_Atomic uint64_t g_lt_enq_ns[RING_SIZE];
_Atomic uint64_t g_lt_deq_ns[RING_SIZE];
_Atomic uint64_t g_lt_run_ns[RING_SIZE];
_Atomic uint32_t g_lt_deq_cnt[RING_SIZE];
_Atomic uint8_t  g_lt_path[RING_SIZE];

_Atomic uint64_t g_lt_dispatch_round_cnt;
_Atomic uint64_t g_lt_send_call_cnt[LAT_TRACE_TYPE_CNT];
_Atomic uint64_t g_lt_send_starve_cnt[LAT_TRACE_TYPE_CNT];
_Atomic uint64_t g_lt_starve_with_work_cnt[LAT_TRACE_TYPE_CNT];
_Atomic uint64_t g_lt_starve_waiters_sum[LAT_TRACE_TYPE_CNT];
_Atomic uint64_t g_lt_free_core_sum[LAT_TRACE_TYPE_CNT];
_Atomic uint64_t g_lt_free_slot0_sum[LAT_TRACE_TYPE_CNT];
_Atomic uint64_t g_lt_free_slot1_sum[LAT_TRACE_TYPE_CNT];
_Atomic uint64_t g_lt_free_allslot_sum[LAT_TRACE_TYPE_CNT];
_Atomic uint64_t g_lt_setmix_call_cnt;
_Atomic uint64_t g_lt_setmix_before_sum;
_Atomic uint64_t g_lt_setmix_after_sum;

void lat_trace_init(void)
{
    for (int i = 0; i < RING_SIZE; i++) {
        atomic_store_explicit(&g_lt_ready_ns[i], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_enq_ns[i], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_deq_ns[i], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_run_ns[i], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_deq_cnt[i], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_path[i], LAT_TRACE_PATH_NONE, memory_order_relaxed);
    }
    atomic_store_explicit(&g_lt_dispatch_round_cnt, 0, memory_order_relaxed);
    for (int t = 0; t < LAT_TRACE_TYPE_CNT; t++) {
        atomic_store_explicit(&g_lt_send_call_cnt[t], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_send_starve_cnt[t], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_starve_with_work_cnt[t], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_starve_waiters_sum[t], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_free_core_sum[t], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_free_slot0_sum[t], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_free_slot1_sum[t], 0, memory_order_relaxed);
        atomic_store_explicit(&g_lt_free_allslot_sum[t], 0, memory_order_relaxed);
    }
    atomic_store_explicit(&g_lt_setmix_call_cnt, 0, memory_order_relaxed);
    atomic_store_explicit(&g_lt_setmix_before_sum, 0, memory_order_relaxed);
    atomic_store_explicit(&g_lt_setmix_after_sum, 0, memory_order_relaxed);
}

void lat_trace_dump(void)
{
    /* TASK_TYPE_CUBE=0；TASK_TYPE_VECTOR 与 TASK_TYPE_MIX 同为 1；下标 2 无人使用 */
    static const char *type_name[LAT_TRACE_TYPE_CNT] = {"cube", "vector(=mix)", "idx2"};
    uint64_t rounds = atomic_load_explicit(&g_lt_dispatch_round_cnt, memory_order_relaxed);
    MAIN_LOGF("[trace] dispatch_rounds = %llu", (unsigned long long)rounds);

    uint64_t mix_calls = atomic_load_explicit(&g_lt_setmix_call_cnt, memory_order_relaxed);
    if (mix_calls > 0) {
        double before = (double)atomic_load_explicit(&g_lt_setmix_before_sum,
                                                     memory_order_relaxed) / (double)mix_calls;
        double after = (double)atomic_load_explicit(&g_lt_setmix_after_sum,
                                                    memory_order_relaxed) / (double)mix_calls;
        MAIN_LOGF("[trace] set_mix: vector_free_mean %.2f -> %.2f (lost %.2f cores/call)",
                  before, after, before - after);
    }
    for (int t = 0; t < LAT_TRACE_TYPE_CNT; t++) {
        uint64_t calls = atomic_load_explicit(&g_lt_send_call_cnt[t], memory_order_relaxed);
        uint64_t starve = atomic_load_explicit(&g_lt_send_starve_cnt[t], memory_order_relaxed);
        uint64_t hungry = atomic_load_explicit(&g_lt_starve_with_work_cnt[t], memory_order_relaxed);
        uint64_t waiters = atomic_load_explicit(&g_lt_starve_waiters_sum[t], memory_order_relaxed);
        uint64_t free_sum = atomic_load_explicit(&g_lt_free_core_sum[t], memory_order_relaxed);
        if (calls == 0) {
            continue;
        }
        /*
         * free_sum 口径 = popcount(所有 slot 的并集)，即 send_task 可派发的核数
         * （任一 slot 空闲即可用，每核每轮一个名额），上限 AIC_CNT。
         * 与旧版要求两 slot 全空的 free_core_mean 不可直接比较。
         */
        double s0m = (double)atomic_load_explicit(&g_lt_free_slot0_sum[t],
                                                  memory_order_relaxed) / (double)calls;
        double s1m = (double)atomic_load_explicit(&g_lt_free_slot1_sum[t],
                                                  memory_order_relaxed) / (double)calls;
        double allm = (double)atomic_load_explicit(&g_lt_free_allslot_sum[t],
                                                   memory_order_relaxed) / (double)calls;
        double anym = (double)free_sum / (double)calls;
        MAIN_LOGF("[trace] %s: calls = %llu, usable_core_mean = %.2f",
                  type_name[t], (unsigned long long)calls, anym);
        /* all_free = 全部 slot 都空闲的核，send_task 优先派这批 */
        MAIN_LOGF("[trace] %s: free slot0 = %.2f, slot1 = %.2f, all_free = %.2f, usable_core = %.2f",
                  type_name[t], s0m, s1m, allm, anym);
        MAIN_LOGF("[trace] %s: starve = %llu (%.2f%%), starve_with_waiters = %llu (%.2f%%)",
                  type_name[t], (unsigned long long)starve,
                  100.0 * (double)starve / (double)calls,
                  (unsigned long long)hungry,
                  100.0 * (double)hungry / (double)calls);
        if (hungry > 0) {
            MAIN_LOGF("[trace] %s: waiters_mean_when_hungry = %.2f",
                      type_name[t], (double)waiters / (double)hungry);
        }
    }

    const char *csv = getenv("LAT_TRACE_CSV");
    if (csv == NULL || csv[0] == '\0') {
        return;
    }
    FILE *fp = fopen(csv, "w");
    if (fp == NULL) {
        MAIN_LOGF("[trace] cannot open %s", csv);
        return;
    }
    fprintf(fp, "task_id,path,ready_ns,enq_ns,deq_ns,run_ns,"
                "d_ready_enq,d_enq_deq,d_deq_run,d_total,deq_cnt\n");
    for (int i = 0; i < RING_SIZE; i++) {
        uint64_t ready = atomic_load_explicit(&g_lt_ready_ns[i], memory_order_relaxed);
        if (ready == 0) {
            continue;
        }
        uint64_t enq = atomic_load_explicit(&g_lt_enq_ns[i], memory_order_relaxed);
        uint64_t deq = atomic_load_explicit(&g_lt_deq_ns[i], memory_order_relaxed);
        uint64_t run = atomic_load_explicit(&g_lt_run_ns[i], memory_order_relaxed);
        uint32_t dcnt = atomic_load_explicit(&g_lt_deq_cnt[i], memory_order_relaxed);
        unsigned path = atomic_load_explicit(&g_lt_path[i], memory_order_relaxed);

        /* 缺失的那一段留空，避免把 0 当成时间戳参与减法 */
        long long d_ready_enq = enq ? (long long)(enq - ready) : -1;
        long long d_enq_deq = (enq && deq) ? (long long)(deq - enq) : -1;
        long long d_deq_run = (deq && run) ? (long long)(run - deq) : -1;
        long long d_total = run ? (long long)(run - ready) : -1;

        fprintf(fp, "%d,%u,%llu,%llu,%llu,%llu,%lld,%lld,%lld,%lld,%u\n",
                i, path,
                (unsigned long long)ready, (unsigned long long)enq,
                (unsigned long long)deq, (unsigned long long)run,
                d_ready_enq, d_enq_deq, d_deq_run, d_total, dcnt);
    }
    fclose(fp);
    MAIN_LOGF("[trace] csv written: %s", csv);
}
#endif
