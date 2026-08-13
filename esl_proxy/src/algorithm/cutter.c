#include "cutter.h"
#include "early_dispatch.h"
#include "lat_trace.h"
#include "log.h"
#include "ring_buf.h"
#include "swimlane.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * ED_PAD2_BYTES：代码布局对照实验专用，默认不定义、零影响。配合 main.c 里的
 * ED_PAD_BYTES 使用，说明见那里。
 *
 * 需要两个独立填充点，是因为 ED_ENABLE=1 让 .text 在两个不同位置变大：
 *   main +850 和 executor_init +27（对齐后合计 +896）—— 平移 executor.o 及其后全部；
 *   dispatch_worker +288 —— 只平移 cutter.o 及其后。
 * 所以要在 ED_ENABLE=0 上复现 ED_ENABLE=1 的地址分布，需要
 * main.c 加 896（推动所有人）+ cutter.c 加 288（只推动 cutter.o 及其后）。
 *
 * 注意 GCC 把顶层 asm 先于函数体输出，所以这段字节总是落在本编译单元 .text
 * 的最前面，与它在源码里的位置无关。
 *
 * 用法：make ED_ENABLE=0 EXTRA_CFLAGS="-DED_PAD_BYTES=896 -DED_PAD2_BYTES=288"
 */
#ifdef ED_PAD2_BYTES
#define ED_PAD2_STR2(x) #x
#define ED_PAD2_STR(x) ED_PAD2_STR2(x)
__asm__(".pushsection .text\n\t"
        ".balign 16\n\t"
        ".globl esl_layout_pad2_never_called\n\t"
        ".type esl_layout_pad2_never_called, @function\n"
        "esl_layout_pad2_never_called:\n\t"
        ".space " ED_PAD2_STR(ED_PAD2_BYTES) ", 0x90\n\t"
        ".size esl_layout_pad2_never_called, .-esl_layout_pad2_never_called\n\t"
        ".popsection");
#endif

task_state* g_state_buf;

void init_state_buf(void) {
    g_state_buf = malloc(sizeof(task_state) * RING_SIZE);
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

uint32_t  g_predecessor_cnt[RING_SIZE];
uint32_t g_commit_task_id = 0;
uint32_t g_completed_task_cnt = 0;

static inline bool update_task_state(uint32_t cnt, uint32_t* cq_buf)
{
    if (cnt <= 0)
        return false;

    for (uint32_t j = 0; j < cnt; j++) {
        uint32_t task_id = cq_buf[j];
        uint16_t idx = (uint16_t)(task_id & RING_MASK);
        if (g_state_buf[idx].task_id == task_id) {
            g_state_buf[idx].state = TASK_STATUS_COMPLETED;
        }
    }

    uint32_t i = (uint32_t)atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
    uint32_t end = (uint32_t)atomic_load_explicit(&g_task_id, memory_order_acquire);
    for (; i < end; i++) {
        uint16_t idx = (uint16_t)(i & RING_MASK);
        if (g_state_buf[idx].task_id != i ||
            g_state_buf[idx].state != TASK_STATUS_COMPLETED) {
            break;
        }
    }
    atomic_store(&g_min_uncomplete_task, (int)i);
    WORKER_LOGF("min_uncomplete_task,%u, completed_cnt,%u, cube_ready_cnt,%d,vector_ready_cnt,%d", \
        (unsigned)atomic_load_explicit(&g_min_uncomplete_task, memory_order_relaxed),
        (unsigned)end,
        g_ctrl_t[0].ready_queue[2].cnt,
        g_ctrl_t[0].ready_queue[1].cnt);

    return true;
}

#if ED_ENABLE && !ED_ABLATE_CUTTER
static inline void cutter_maybe_enter_staging(uint32_t s_full, uint16_t s_idx, uint16_t fanin_now)
{
    if (fanin_now != g_dispatch_fanin_target[s_idx]) {
        return;
    }
    /*
     * 与 ed_maybe_enter_staging 必须同口径，故共用 ed_count_admitted()。
     * 两个 staging 入口若准入条件不一致，覆盖率会随线程时序抖动、难复现。
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
        if (!enqueue(&g_ed_ready_queue, s_full)) {
            uint8_t staged = ED_SPEC_STAGING;
            (void)atomic_compare_exchange_strong_explicit(
                &g_spec_state[s_idx], &staged, ED_SPEC_NONE,
                memory_order_acq_rel, memory_order_acquire);
        }
    }
}
#endif

void add_successors(uint32_t ready_cnt[], uint32_t rq_buf[][RQ_BATCH_SIZE]) {
    uint32_t end = atomic_load(&g_task_id);
    uint32_t tmp = g_commit_task_id + PRE_BATCH_SIZE;
    end = tmp > end ? end : tmp;
    /* 右开窗口 [g_commit_task_id, end)：g_task_id 是下一待分配 ID，不能提交等于 end 的 slot。 */
    while (g_commit_task_id < end)
    {
        uint32_t s_full = g_commit_task_id;
        uint16_t s_idx = (uint16_t)(s_full & RING_MASK);
        struct predecessor_list *ptr = &g_predecessors[s_idx];
        task_type_t s_type = g_basic_buf[s_idx].type;

        if (ptr->cnt <= 0) {
#if !ED_ABLATE_CUTTER
            ed_init_task_meta(s_full, 0);
#endif
            if (g_state_buf != NULL) {
                g_state_buf[s_idx].task_id = s_full;
                g_state_buf[s_idx].state = TASK_STATUS_SUBMITTED;
                g_state_buf[s_idx].successor_cnt = 0;
            }
            g_predecessor_cnt[s_idx] = 0;
            rq_buf[s_type][ready_cnt[s_type]++] = s_full;
            g_commit_task_id++;
            WORKER_LOGF("ready_cnt[%d],%d", s_type, ready_cnt[s_type]);
            continue;
        }

        uint16_t original_cnt = (uint16_t)ptr->cnt;
        uint16_t predecessor_cnt = 0;
        struct {
            uint32_t p_full;
            uint16_t p_idx;
        } survivors[CON_NODE_CNT];

        /* 第一趟：本地扫描 pred，不进锁，不写 successor list */
        for (uint16_t k = 0; k < original_cnt; k++) {
            uint32_t p_full = ptr->exp[k];
            uint16_t p_idx = (uint16_t)(p_full & RING_MASK);
            if (g_state_buf != NULL) {
                uint32_t tracked_task = g_state_buf[p_idx].task_id;
                if (tracked_task != p_full &&
                    tracked_task != 0 &&
                    atomic_load_explicit(&g_ring_task_tag[p_idx], memory_order_acquire) !=
                        p_full) {
                    continue;
                }
                if (g_state_buf[p_idx].state == TASK_STATUS_COMPLETED) {
                    continue;
                }
            }
            if (predecessor_cnt < CON_NODE_CNT) {
                survivors[predecessor_cnt].p_full = p_full;
                survivors[predecessor_cnt].p_idx = p_idx;
                predecessor_cnt++;
            }
        }

        /* 关键屏障：先固化 s 元数据，再 append 边，避免 Hook0 读到旧 target。 */
#if !ED_ABLATE_CUTTER
        ed_init_task_meta(s_full, predecessor_cnt);
#endif
        if (g_state_buf != NULL) {
            g_state_buf[s_idx].task_id = s_full;
            g_state_buf[s_idx].state = TASK_STATUS_SUBMITTED;
            g_state_buf[s_idx].successor_cnt = 0;
        }
        g_predecessor_cnt[s_idx] = predecessor_cnt;

#if ED_ENABLE && !ED_ABLATE_CUTTER
        for (uint16_t k = 0; k < predecessor_cnt; k++) {
            g_ed_pred_snapshot[s_idx].node[k] = survivors[k].p_full;
        }
        g_ed_pred_snapshot[s_idx].cnt = predecessor_cnt;
#endif

        if (predecessor_cnt == 0) {
            rq_buf[s_type][ready_cnt[s_type]++] = s_full;
            WORKER_LOGF("ready_cnt[%d],%d", s_type, ready_cnt[s_type]);
            ptr->exp += original_cnt;
            ptr->cnt = 0;
            g_commit_task_id++;
            continue;
        }

        /* 第二趟：锁内 append，命中 dispatch_tag 时补 late-arrival fanin。 */
        for (uint16_t k = 0; k < predecessor_cnt; k++) {
            uint16_t p_idx = survivors[k].p_idx;

#if ED_ENABLE && !ED_ABLATE_CUTTER
            /* p_full 只参与 generation tag 比对，ED 关闭时无使用者 */
            uint32_t p_full = survivors[k].p_full;

            ed_edge_lock(p_idx);
            if (atomic_load_explicit(&g_ring_task_tag[p_idx], memory_order_acquire) !=
                p_full) {
                ed_edge_unlock(p_idx);

                uint16_t old_unfin = atomic_fetch_sub_explicit(
                    &g_unfin_pred_cnt[s_idx], 1, memory_order_acq_rel);
                if (old_unfin > 0) {
                    uint16_t fanin_now = (uint16_t)(atomic_fetch_add_explicit(
                                             &g_dispatch_fanin[s_idx], 1,
                                             memory_order_acq_rel) +
                                         1);
                    /* 记账口径随准入放开：count>1 的 late arrival 也要统计，
                     * 否则指标恰好在新增人群上失明。 */
                    atomic_fetch_add_explicit(&g_ed_late_arrival_cnt, 1, memory_order_relaxed);
                    cutter_maybe_enter_staging(s_full, s_idx, fanin_now);
                }
                continue;
            }

            uint32_t successor_idx = g_successor_buf[p_idx].cnt;
            g_successor_buf[p_idx].node[successor_idx] = s_full;
            g_successor_buf[p_idx].cnt = successor_idx + 1;
            if (g_state_buf != NULL) {
                g_state_buf[p_idx].successor_cnt++;
            }

            bool dispatched_this_gen =
                atomic_load_explicit(&g_dispatch_tag[p_idx], memory_order_acquire) ==
                p_full;
            ed_edge_unlock(p_idx);

            if (!dispatched_this_gen) {
                continue;
            }

            uint16_t fanin_now = (uint16_t)(atomic_fetch_add_explicit(
                                     &g_dispatch_fanin[s_idx], 1,
                                     memory_order_acq_rel) +
                                 1);
            atomic_fetch_add_explicit(&g_ed_late_arrival_cnt, 1, memory_order_relaxed);
            cutter_maybe_enter_staging(s_full, s_idx, fanin_now);
#else
            uint32_t successor_idx = g_successor_buf[p_idx].cnt;
            g_successor_buf[p_idx].node[successor_idx] = s_full;
            g_successor_buf[p_idx].cnt = successor_idx + 1;
            if (g_state_buf != NULL) {
                g_state_buf[p_idx].successor_cnt++;
            }
#endif
        }

        ptr->exp += original_cnt;
        ptr->cnt = 0;
        g_commit_task_id++;
    }
}

void send_2_ready_queue(uint32_t ready_cnt[], uint32_t rq_buf[][RQ_BATCH_SIZE]) {
    for (uint32_t j = 0; j < 2; j++) {
        int target_ctrl = 0;
        queue_t *rq = &g_ctrl_t[target_ctrl].ready_queue[j];
        if (ready_cnt[j] > 0)
        {
            WORKER_LOGF("batch_enqueue,%d,cnt,%u,first,%d",j, ready_cnt[j], rq_buf[j][0]);
            for (uint16_t k = 0; k < ready_cnt[j]; k++) {
                lat_trace_enq(rq_buf[j][k]);
            }
            batch_enqueue(rq, rq_buf[j], ready_cnt[j]);
        }
    }
}

void resolve_dep(uint32_t cnt, uint32_t* cq_buf, uint32_t rq_buf[][RQ_BATCH_SIZE], uint32_t* ready_cnt) {
    uint32_t task_id;
    uint32_t succ_id;
    uint32_t idx;
    uint32_t succ_cnt;
    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        idx = task_id & RING_MASK;
        task_state st = g_state_buf[idx];
        succ_cnt = (uint32_t)st.successor_cnt;
        WORKER_LOGF("completed,task_id,%u,type,%u, successor_cnt,%u", task_id, g_basic_buf[idx].type, succ_cnt);
        for (uint32_t k = 0; k < succ_cnt; k++) {
            succ_id = g_successor_buf[idx].node[k];
            uint16_t s_idx = (uint16_t)(succ_id & RING_MASK);
            g_predecessor_cnt[s_idx]--;
#if ED_ENABLE && !ED_ABLATE_CUTTER
            uint16_t old_unfin = atomic_fetch_sub_explicit(
                &g_unfin_pred_cnt[s_idx], 1, memory_order_acq_rel);
            assert(old_unfin > 0);
#endif
            WORKER_LOGF("cutter, task_id,%u, successor_id,%u, predecessor_cnt,%u", task_id, succ_id, g_predecessor_cnt[s_idx]);
            if (g_predecessor_cnt[s_idx] < 1) {
                task_type_t type = g_basic_buf[s_idx].type;
                /*
                 * KPI 起点：依赖刚满足。必须早于下面的 Hook2 通知，
                 * 否则 ED 路径量到的延迟会退化成 0。
                 */
                ed_lat_mark_ready(succ_id);
                lat_trace_ready(succ_id);
                swim_mark(succ_id, SWIM_MK_READY);
#if ED_ENABLE && !ED_ABLATE_CUTTER
                /* Step 6 Hook 2：1->0 线程统一切到 DISPATCHED，并通过 notify_once 竞争唯一通知。 */
                assert(old_unfin == 1);
                uint8_t old = atomic_exchange_explicit(
                    &g_spec_state[s_idx], ED_SPEC_DISPATCHED, memory_order_seq_cst);
                if (old == ED_SPEC_STAGING) {
                    uint64_t record = atomic_load_explicit(&g_staged_slot_record[s_idx],
                                                           memory_order_seq_cst);
                    if (ed_record_tag_matches(record, succ_id) &&
                        ED_UNPACK_CORE(ED_RECORD_SLOT(record)) != ED_STAGED_CORE_INVALID) {
                        ed_notify_once(succ_id, record, ED_NOTIFY_HOOK2);
                    }
                } else if (old == ED_SPEC_DISPATCHED) {
                    WORKER_LOGF("[ed] BUG: Hook2 fired twice for s=%u", succ_id);
                }
#endif
                rq_buf[type][ready_cnt[type]] = succ_id;
                ready_cnt[type]++;
                WORKER_LOGF("ready_cnt[%d],%d",type, ready_cnt[type]);
            }
        }
    }
}

void deal_completed_queue(void) {
    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        uint32_t cq_buf[CQ_BATCH_SIZE];
        uint32_t rq_buf[2][RQ_BATCH_SIZE];
        uint32_t ready_cnt[2] = {0, 0};
        queue_t *cq = &g_ctrl_t[i].completed_queue;
        uint32_t cnt = CQ_BATCH_SIZE;
        batch_dequeue(cq, cq_buf, &cnt);
        g_completed_task_cnt += cnt;
        // for (size_t i = 0; i < cnt; i++)
        // {
        //     WORKER_LOGF("cutter, completed_task_id,%d ", cq_buf[i]);
        // }
        update_task_state(cnt, cq_buf);

        /*
         * 只在提交游标真前进 / 真有完成事件时落阶段条，理由同 dispatcher 侧。
         * 泳道号用的是本轮处理的 dispatcher 队列号（也就是循环变量 i），不是
         * cutter 线程号——deal_completed_queue 拿不到后者。当前只支持
         * CUTTER_THREAD_CNT=1，两者恒为 0；真要多 cutter 线程时得把线程号传进来。
         */
        uint32_t commit_before = g_commit_task_id;
        uint64_t swim_commit_start = swim_now();
        add_successors(ready_cnt, rq_buf);
        if (g_commit_task_id != commit_before) {
            swim_phase(SWIM_ROLE_CUTTER, SWIM_PH_CUT_COMMIT, i, 0,
                       g_commit_task_id - commit_before, swim_commit_start, swim_now());
        }

        uint64_t swim_resolve_start = swim_now();
        resolve_dep(cnt, cq_buf, rq_buf, ready_cnt);
        send_2_ready_queue(ready_cnt, rq_buf);
        if (cnt > 0) {
            swim_phase(SWIM_ROLE_CUTTER, SWIM_PH_CUT_RESOLVE, i, 0, cnt,
                       swim_resolve_start, swim_now());
        }
    }
}

void *cutter_worker(void *arg)
{
    (void)arg;
    init_state_buf();
    while (!atomic_load(&g_is_done)) {
        deal_completed_queue();
    }

    while(g_commit_task_id < (uint32_t)atomic_load(&g_task_id)){
        deal_completed_queue();
    }
    WORKER_LOGF("cutter, commit_tasks_cnt,%d,completed_task_cnt,%d ", g_commit_task_id, g_completed_task_cnt);
    return NULL;
}