#ifndef ALGORITHM_SWIMLANE_H
#define ALGORITHM_SWIMLANE_H

/*
 * 泳道图打点（可视化用，默认关闭）。
 *
 * 目标：把「谁在什么时候占着哪个核」画出来。executor 是 AI core 的软件替身，
 * 每个 (type, core) 就是一条泳道；orch / cutter / dispatcher 各占一条控制面
 * 泳道，落的是阶段条而不是任务条。产物是一份 JSON，由
 * tools/swimlane_to_perfetto.py 转成 Perfetto 可加载的 trace。
 *
 * 一个任务在泳道上分四段，时间点全部取自 get_time_ns_hires()：
 *   stage -> pub    ED 占槽等门铃（GATED）；normal 路径没有这一段
 *   pub   -> run    已可执行但核还没轮到它（预装载位上排队）
 *   run   -> end    executor 认领槽位后真正 tick duration
 *   end   -> finish dispatcher 观察到 done bit 之前的那段回收延迟
 *
 * 为什么 pub 和 run 要分开：ED 的门铃开闸只是把 slot_state 翻成 RUNNABLE，
 * 该核当时可能还在跑另一个 slot，真正开跑要等 executor 主扫描认领。两点合一
 * 会把「开闸慢」和「核忙」混成一笔账，而这恰恰是要看的东西。
 *
 * ===== 开销声明 =====
 * SWIMLANE=1 会在热路径插入时钟读取与 GM 写入，并改变 dispatch/executor 的
 * 代码布局与栈帧。conf.h 与 dispatch.c 里记着「48 字节栈帧平移值 9%~13%
 * makespan」的教训，所以本开关下测出的 makespan 一律不可用于性能对比，
 * 只能用来看结构。性能数据请在 SWIMLANE=0 下采。
 *
 * 开关：make SWIMLANE=1；输出路径由环境变量 SWIMLANE_JSON 指定，
 * 默认 report/swimlane_records.json。
 */

#include <stdint.h>

#include "conf.h"
#include "log.h" /* get_time_ns_hires */

#ifndef SWIMLANE
#define SWIMLANE 0
#endif

/* 任务走的派发路径 */
#define SWIM_PATH_NORMAL 0u
#define SWIM_PATH_ED     1u

/* 控制面角色；取值必须与 swimlane.c 的 g_swim_role_names[] 下标一致 */
enum {
    SWIM_ROLE_ORCH = 0,
    SWIM_ROLE_CUTTER,
    SWIM_ROLE_DISPATCH,
    SWIM_ROLE_CNT
};

/* 阶段条种类；取值必须与 swimlane.c 的 g_swim_phase_names[] 下标一致 */
enum {
    SWIM_PH_ORCH = 0,      /* 整段 aicpu_orchestration_entry */
    SWIM_PH_CUT_COMMIT,    /* add_successors 提交新任务的边 */
    SWIM_PH_CUT_RESOLVE,   /* resolve_dep 递减依赖 */
    SWIM_PH_DISP_DRAIN,    /* drain_completed_snapshot 收完成位 */
    SWIM_PH_DISP_SEND,     /* send_task 正常派发（sub = task type） */
    SWIM_PH_DISP_ED,       /* try_early_dispatch 投机占槽 */
    SWIM_PH_CNT
};

/* 瞬时事件种类；取值必须与 swimlane.c 的 g_swim_mark_names[] 下标一致 */
enum {
    SWIM_MK_READY = 0,     /* 依赖归零 */
    SWIM_MK_NOTIFY_H1,     /* stager 自敲门铃 */
    SWIM_MK_NOTIFY_H2,     /* cutter 依赖归零时敲门铃 */
    SWIM_MK_CNT
};

#if SWIMLANE

#include <stdatomic.h>
#include <stdbool.h>

/*
 * 缓冲上限。溢出不是错误，只丢新记录并计数，dump 时打印，
 * 免得一个大 case 把可视化整个搞崩。
 */
#ifndef SWIM_MAX_TASKS
#define SWIM_MAX_TASKS (1u << 18)
#endif
#ifndef SWIM_MAX_PHASES
#define SWIM_MAX_PHASES (1u << 19)
#endif
#ifndef SWIM_MAX_MARKS
#define SWIM_MAX_MARKS (1u << 19)
#endif

#define SWIM_REC_NONE UINT32_MAX

/* 一条完整的任务执行记录，executor 完成时追加，dispatcher 稍后回填 finish_ns */
typedef struct {
    uint32_t task_id;
    uint16_t core;
    uint8_t type;   /* 0=CUBE 1=VECTOR，与 EXE_TYPE 同义 */
    uint8_t path;   /* SWIM_PATH_* */
    uint32_t blocks; /* SPMD 块数（g_basic_buf[].count） */
    /*
     * 泳道的最小单位是槽位而不是核：ED 把任务 stage 到某核的空闲 slot 上等门铃时，
     * 同一个核的另一个 slot 还在跑别的任务，两段时间是重叠的。按核画会撞在一起，
     * 按槽位画则天然互不重叠——一个 slot 同时只承载一个任务，且下一个任务要等
     * dispatcher drain 掉上一个（free bit 在那时才归还）才可能占进来。
     */
    uint32_t slot;
    uint64_t stage_ns;  /* ED 发布 GATED 的时刻；normal 恒为 0 */
    uint64_t pub_ns;    /* 任务变为可执行（normal 发布 RUNNABLE / ED 门铃开闸） */
    uint64_t run_ns;    /* executor 认领该槽位 */
    uint64_t end_ns;    /* complete_slot */
    _Atomic uint64_t finish_ns; /* dispatcher drain 到它；0 = 未回填 */
} swim_task_rec_t;

typedef struct {
    uint8_t role;
    uint8_t kind;
    uint8_t tid;   /* 产生该阶段的 cutter / dispatch 线程号，决定落在哪条泳道 */
    uint8_t sub;   /* SWIM_PH_DISP_SEND 用它放 task type，其余为 0 */
    uint32_t n;    /* 本阶段处理的任务数 */
    uint64_t start_ns;
    uint64_t end_ns;
} swim_phase_rec_t;

typedef struct {
    uint32_t task_id;
    uint32_t kind; /* SWIM_MK_* */
    uint64_t ns;
} swim_mark_rec_t;

/*
 * 按 ring 下标存的临时槽。任务从派发到完成期间，四个时间点由 dispatcher 与
 * executor 两个线程分别写入，写完才由 executor 拼成一条 swim_task_rec_t 追加
 * 到 g_swim_tasks。ring 复用不成问题——同一下标的下一代任务必然在上一代完成
 * 之后才写入，而上一代此时已经落盘。
 *
 * task_id 存的是完整 32 位值，不能靠 executor 侧那个被截成 uint16 的局部量：
 * 任务数超过 65535 时它会回绕，用来做换代校验会误判。
 */
typedef struct {
    _Atomic uint32_t task_id;
    _Atomic uint32_t rec_idx; /* executor 追加后写回，供 dispatcher 回填 finish */
    _Atomic uint32_t blocks;
    _Atomic uint16_t core;
    _Atomic uint8_t type;
    _Atomic uint8_t path;
    _Atomic uint32_t slot;
    _Atomic uint64_t stage_ns;
    _Atomic uint64_t pub_ns;
    _Atomic uint64_t run_ns;
} swim_scratch_t;

extern bool g_swim_on;
extern uint64_t g_swim_t0_ns;
extern swim_scratch_t *g_swim_scratch;
extern swim_task_rec_t *g_swim_tasks;
extern swim_phase_rec_t *g_swim_phases;
extern swim_mark_rec_t *g_swim_marks;
extern _Atomic uint32_t g_swim_task_cnt;
extern _Atomic uint32_t g_swim_phase_cnt;
extern _Atomic uint32_t g_swim_mark_cnt;
extern _Atomic uint32_t g_swim_task_drop;
extern _Atomic uint32_t g_swim_phase_drop;
extern _Atomic uint32_t g_swim_mark_drop;

void swim_init(void);
void swim_dump(void);

static inline uint64_t swim_now(void)
{
    return g_swim_on ? get_time_ns_hires() : 0;
}

/*
 * normal 路径入口：必须在发布 RUNNABLE 之前调用。
 * 反过来写会有真实的丢点窗口——executor 是另一个线程，槽位一变 RUNNABLE
 * 它下一拍就可能认领甚至跑完，那时 scratch 还是上一代的内容。
 */
static inline void swim_task_publish(uint32_t task_id, int core, int type, int slot,
                                     uint32_t blocks)
{
    if (!g_swim_on) {
        return;
    }
    swim_scratch_t *s = &g_swim_scratch[task_id & RING_MASK];
    atomic_store_explicit(&s->core, (uint16_t)core, memory_order_relaxed);
    atomic_store_explicit(&s->type, (uint8_t)type, memory_order_relaxed);
    atomic_store_explicit(&s->slot, (uint32_t)slot, memory_order_relaxed);
    atomic_store_explicit(&s->path, (uint8_t)SWIM_PATH_NORMAL, memory_order_relaxed);
    atomic_store_explicit(&s->blocks, blocks, memory_order_relaxed);
    atomic_store_explicit(&s->stage_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->run_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->rec_idx, SWIM_REC_NONE, memory_order_relaxed);
    atomic_store_explicit(&s->pub_ns, get_time_ns_hires(), memory_order_relaxed);
    /* task_id 最后写：读侧以它作为「本代数据已齐」的标志 */
    atomic_store_explicit(&s->task_id, task_id, memory_order_release);
}

/* ED 路径入口：必须在发布 GATED 之前调用，理由同上 */
static inline void swim_task_stage(uint32_t task_id, int core, int type, int slot,
                                   uint32_t blocks)
{
    if (!g_swim_on) {
        return;
    }
    swim_scratch_t *s = &g_swim_scratch[task_id & RING_MASK];
    atomic_store_explicit(&s->core, (uint16_t)core, memory_order_relaxed);
    atomic_store_explicit(&s->type, (uint8_t)type, memory_order_relaxed);
    atomic_store_explicit(&s->slot, (uint32_t)slot, memory_order_relaxed);
    atomic_store_explicit(&s->path, (uint8_t)SWIM_PATH_ED, memory_order_relaxed);
    atomic_store_explicit(&s->blocks, blocks, memory_order_relaxed);
    atomic_store_explicit(&s->pub_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->run_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->rec_idx, SWIM_REC_NONE, memory_order_relaxed);
    atomic_store_explicit(&s->stage_ns, get_time_ns_hires(), memory_order_relaxed);
    atomic_store_explicit(&s->task_id, task_id, memory_order_release);
}

/* ED 门铃开闸：GATED -> RUNNABLE */
static inline void swim_task_ungate(uint32_t task_id)
{
    if (!g_swim_on) {
        return;
    }
    swim_scratch_t *s = &g_swim_scratch[task_id & RING_MASK];
    if (atomic_load_explicit(&s->task_id, memory_order_acquire) != task_id) {
        return;
    }
    atomic_store_explicit(&s->pub_ns, get_time_ns_hires(), memory_order_relaxed);
}

/* executor 认领槽位，真正开跑 */
static inline void swim_task_run(uint32_t task_id)
{
    if (!g_swim_on) {
        return;
    }
    swim_scratch_t *s = &g_swim_scratch[task_id & RING_MASK];
    if (atomic_load_explicit(&s->task_id, memory_order_acquire) != task_id) {
        return;
    }
    atomic_store_explicit(&s->run_ns, get_time_ns_hires(), memory_order_relaxed);
}

/* executor 完成：把 scratch 拼成一条记录追加进去 */
static inline void swim_task_end(uint32_t task_id)
{
    if (!g_swim_on) {
        return;
    }
    swim_scratch_t *s = &g_swim_scratch[task_id & RING_MASK];
    if (atomic_load_explicit(&s->task_id, memory_order_acquire) != task_id) {
        return;
    }
    uint32_t idx = atomic_fetch_add_explicit(&g_swim_task_cnt, 1, memory_order_relaxed);
    if (idx >= SWIM_MAX_TASKS) {
        atomic_fetch_add_explicit(&g_swim_task_drop, 1, memory_order_relaxed);
        return;
    }
    swim_task_rec_t *r = &g_swim_tasks[idx];
    r->task_id = task_id;
    r->core = atomic_load_explicit(&s->core, memory_order_relaxed);
    r->type = atomic_load_explicit(&s->type, memory_order_relaxed);
    r->path = atomic_load_explicit(&s->path, memory_order_relaxed);
    r->blocks = atomic_load_explicit(&s->blocks, memory_order_relaxed);
    r->slot = atomic_load_explicit(&s->slot, memory_order_relaxed);
    r->stage_ns = atomic_load_explicit(&s->stage_ns, memory_order_relaxed);
    r->pub_ns = atomic_load_explicit(&s->pub_ns, memory_order_relaxed);
    r->run_ns = atomic_load_explicit(&s->run_ns, memory_order_relaxed);
    r->end_ns = get_time_ns_hires();
    atomic_store_explicit(&r->finish_ns, 0, memory_order_relaxed);
    /* release：dispatcher 靠 acquire 读到这个下标后才去写 finish_ns */
    atomic_store_explicit(&s->rec_idx, idx, memory_order_release);
}

/* dispatcher 收到 done bit，回填到 executor 留下的那条记录上 */
static inline void swim_task_finish(uint32_t task_id)
{
    if (!g_swim_on) {
        return;
    }
    swim_scratch_t *s = &g_swim_scratch[task_id & RING_MASK];
    uint32_t idx = atomic_load_explicit(&s->rec_idx, memory_order_acquire);
    if (idx == SWIM_REC_NONE || idx >= SWIM_MAX_TASKS) {
        return;
    }
    /*
     * 换代校验：complete 到 drain 之间理论上 ring 可以转满一圈把下标复用掉，
     * 那样就会把 finish 写到别人的记录上。比对记录里的 task_id 即可挡住。
     */
    if (g_swim_tasks[idx].task_id != task_id) {
        return;
    }
    atomic_store_explicit(&g_swim_tasks[idx].finish_ns, get_time_ns_hires(),
                          memory_order_relaxed);
}

/*
 * 阶段条。只在真干了活时调用——dispatch 主循环是百万次量级，
 * 每轮都落一条会直接把内存和 Perfetto 撑爆，空转轮由相邻两条记录之间的
 * 空隙表示即可。
 */
static inline void swim_phase(int role, int kind, int tid, int sub, uint32_t n,
                              uint64_t start_ns, uint64_t end_ns)
{
    if (!g_swim_on) {
        return;
    }
    uint32_t idx = atomic_fetch_add_explicit(&g_swim_phase_cnt, 1, memory_order_relaxed);
    if (idx >= SWIM_MAX_PHASES) {
        atomic_fetch_add_explicit(&g_swim_phase_drop, 1, memory_order_relaxed);
        return;
    }
    swim_phase_rec_t *r = &g_swim_phases[idx];
    r->role = (uint8_t)role;
    r->kind = (uint8_t)kind;
    r->tid = (uint8_t)tid;
    r->sub = (uint8_t)sub;
    r->n = n;
    r->start_ns = start_ns;
    r->end_ns = end_ns;
}

static inline void swim_mark(uint32_t task_id, int kind)
{
    if (!g_swim_on) {
        return;
    }
    uint32_t idx = atomic_fetch_add_explicit(&g_swim_mark_cnt, 1, memory_order_relaxed);
    if (idx >= SWIM_MAX_MARKS) {
        atomic_fetch_add_explicit(&g_swim_mark_drop, 1, memory_order_relaxed);
        return;
    }
    g_swim_marks[idx].task_id = task_id;
    g_swim_marks[idx].kind = (uint32_t)kind;
    g_swim_marks[idx].ns = get_time_ns_hires();
}

#else /* !SWIMLANE */

static inline void swim_init(void) {}
static inline void swim_dump(void) {}
static inline uint64_t swim_now(void) { return 0; }

static inline void swim_task_publish(uint32_t task_id, int core, int type, int slot,
                                     uint32_t blocks)
{
    (void)task_id;
    (void)core;
    (void)type;
    (void)slot;
    (void)blocks;
}
static inline void swim_task_stage(uint32_t task_id, int core, int type, int slot,
                                   uint32_t blocks)
{
    (void)task_id;
    (void)core;
    (void)type;
    (void)slot;
    (void)blocks;
}
static inline void swim_task_ungate(uint32_t task_id) { (void)task_id; }
static inline void swim_task_run(uint32_t task_id) { (void)task_id; }
static inline void swim_task_end(uint32_t task_id) { (void)task_id; }
static inline void swim_task_finish(uint32_t task_id) { (void)task_id; }
static inline void swim_phase(int role, int kind, int tid, int sub, uint32_t n,
                              uint64_t start_ns, uint64_t end_ns)
{
    (void)role;
    (void)kind;
    (void)tid;
    (void)sub;
    (void)n;
    (void)start_ns;
    (void)end_ns;
}
static inline void swim_mark(uint32_t task_id, int kind)
{
    (void)task_id;
    (void)kind;
}

#endif /* SWIMLANE */

#endif /* ALGORITHM_SWIMLANE_H */
