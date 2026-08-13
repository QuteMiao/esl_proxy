/*
 * swimlane.c - 泳道图记录缓冲与 JSON 落盘
 *
 * 打点接口全部是 swimlane.h 里的 static inline，本文件只负责三件事：
 * 分配缓冲、记住 t0、把三条流写成 JSON。
 *
 * 落盘发生在所有工作线程 join 之后（main.c），所以 dump 不需要任何同步。
 */

#define _POSIX_C_SOURCE 200809L

#include "swimlane.h"

#if SWIMLANE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

bool g_swim_on = false;
uint64_t g_swim_t0_ns = 0;
swim_scratch_t *g_swim_scratch = NULL;
swim_task_rec_t *g_swim_tasks = NULL;
swim_phase_rec_t *g_swim_phases = NULL;
swim_mark_rec_t *g_swim_marks = NULL;
_Atomic uint32_t g_swim_task_cnt = 0;
_Atomic uint32_t g_swim_phase_cnt = 0;
_Atomic uint32_t g_swim_mark_cnt = 0;
_Atomic uint32_t g_swim_task_drop = 0;
_Atomic uint32_t g_swim_phase_drop = 0;
_Atomic uint32_t g_swim_mark_drop = 0;

/* 下标必须与 swimlane.h 的同名枚举一致；转换器直接按下标取名 */
static const char *const g_swim_role_names[SWIM_ROLE_CNT] = {
    "orch", "cutter", "dispatch"
};
static const char *const g_swim_phase_names[SWIM_PH_CNT] = {
    "orch_run", "cut_commit", "cut_resolve", "disp_drain", "disp_send", "disp_ed"
};
static const char *const g_swim_mark_names[SWIM_MK_CNT] = {
    "ready", "notify_h1", "notify_h2"
};

#define SWIM_STR2(x) #x
#define SWIM_STR(x) SWIM_STR2(x)

#ifdef ORCH_CASE
#define SWIM_CASE_NAME SWIM_STR(ORCH_CASE)
#else
#define SWIM_CASE_NAME "unknown"
#endif

#define SWIM_DEFAULT_JSON "report/swimlane_records.json"

void swim_init(void)
{
    g_swim_scratch = calloc(RING_SIZE, sizeof *g_swim_scratch);
    g_swim_tasks = calloc(SWIM_MAX_TASKS, sizeof *g_swim_tasks);
    g_swim_phases = calloc(SWIM_MAX_PHASES, sizeof *g_swim_phases);
    g_swim_marks = calloc(SWIM_MAX_MARKS, sizeof *g_swim_marks);

    if (g_swim_scratch == NULL || g_swim_tasks == NULL || g_swim_phases == NULL ||
        g_swim_marks == NULL) {
        fprintf(stderr, "[swimlane] buffer alloc failed, profiling disabled\n");
        free(g_swim_scratch);
        free(g_swim_tasks);
        free(g_swim_phases);
        free(g_swim_marks);
        g_swim_scratch = NULL;
        g_swim_tasks = NULL;
        g_swim_phases = NULL;
        g_swim_marks = NULL;
        return;
    }

    /*
     * calloc 把 rec_idx 清成 0，而 0 是一个合法的记录下标。不预置成 NONE 的话，
     * 一个还没跑完的任务会被 dispatcher 当成「已落盘」，把 finish 写到 0 号记录上。
     */
    for (int i = 0; i < RING_SIZE; i++) {
        atomic_store_explicit(&g_swim_scratch[i].rec_idx, SWIM_REC_NONE,
                              memory_order_relaxed);
        atomic_store_explicit(&g_swim_scratch[i].task_id, UINT32_MAX,
                              memory_order_relaxed);
    }

    g_swim_t0_ns = get_time_ns_hires();
    g_swim_on = true;
}

/* 相对 t0 的纳秒；时钟回拨或未打点的 0 一律归零，转换器按 0 判缺失 */
static uint64_t rel_ns(uint64_t ns)
{
    if (ns == 0 || ns < g_swim_t0_ns) {
        return 0;
    }
    return ns - g_swim_t0_ns;
}

/* 逐级建目录，等价于 mkdir -p；只处理输出路径的父目录 */
static void mkdir_parents(const char *path)
{
    char buf[512];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof buf) {
        return;
    }
    memcpy(buf, path, len + 1);

    char *slash = strrchr(buf, '/');
    if (slash == NULL) {
        return;
    }
    *slash = '\0';

    for (char *p = buf + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        (void)mkdir(buf, 0755);
        *p = '/';
    }
    (void)mkdir(buf, 0755);
}

static void dump_names(FILE *f, const char *key, const char *const *names, int cnt)
{
    fprintf(f, "  \"%s\": [", key);
    for (int i = 0; i < cnt; i++) {
        fprintf(f, "%s\"%s\"", (i == 0) ? "" : ", ", names[i]);
    }
    fprintf(f, "],\n");
}

void swim_dump(void)
{
    if (!g_swim_on) {
        return;
    }
    g_swim_on = false;

    const char *path = getenv("SWIMLANE_JSON");
    if (path == NULL || path[0] == '\0') {
        path = SWIM_DEFAULT_JSON;
    }
    mkdir_parents(path);

    FILE *f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "[swimlane] cannot write %s: %s\n", path, strerror(errno));
        return;
    }

    uint32_t task_cnt = atomic_load(&g_swim_task_cnt);
    uint32_t phase_cnt = atomic_load(&g_swim_phase_cnt);
    uint32_t mark_cnt = atomic_load(&g_swim_mark_cnt);
    if (task_cnt > SWIM_MAX_TASKS) {
        task_cnt = SWIM_MAX_TASKS;
    }
    if (phase_cnt > SWIM_MAX_PHASES) {
        phase_cnt = SWIM_MAX_PHASES;
    }
    if (mark_cnt > SWIM_MAX_MARKS) {
        mark_cnt = SWIM_MAX_MARKS;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"meta\": {\n");
    fprintf(f, "    \"case\": \"%s\",\n", SWIM_CASE_NAME);
    fprintf(f, "    \"aic_cnt\": %d,\n", AIC_CNT);
    fprintf(f, "    \"exe_type_cnt\": %d,\n", EXE_TYPE_CNT);
    fprintf(f, "    \"aic_ostd\": %d,\n", AIC_OSTD);
    fprintf(f, "    \"ed_enable\": %d,\n", ED_ENABLE);
    fprintf(f, "    \"exec_duration_scale\": %u,\n", (unsigned)EXEC_DURATION_SCALE);
    fprintf(f, "    \"cutter_threads\": %d,\n", CUTTER_THREAD_CNT);
    fprintf(f, "    \"dispatch_threads\": %d,\n", DISPATCH_THREAD_CNT);
    fprintf(f, "    \"executor_threads\": %d\n", EXECUTOR_THREAD_CNT);
    fprintf(f, "  },\n");

    dump_names(f, "role_names", g_swim_role_names, SWIM_ROLE_CNT);
    dump_names(f, "phase_names", g_swim_phase_names, SWIM_PH_CNT);
    dump_names(f, "mark_names", g_swim_mark_names, SWIM_MK_CNT);
    fprintf(f, "  \"path_names\": [\"normal\", \"ed\"],\n");

    /* [task_id, core, type, slot, path, blocks, stage, pub, run, end, finish] */
    fprintf(f, "  \"tasks\": [\n");
    for (uint32_t i = 0; i < task_cnt; i++) {
        const swim_task_rec_t *r = &g_swim_tasks[i];
        fprintf(f, "    [%u,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%llu]%s\n",
                r->task_id, (unsigned)r->core, (unsigned)r->type, r->slot,
                (unsigned)r->path, r->blocks,
                (unsigned long long)rel_ns(r->stage_ns),
                (unsigned long long)rel_ns(r->pub_ns),
                (unsigned long long)rel_ns(r->run_ns),
                (unsigned long long)rel_ns(r->end_ns),
                (unsigned long long)rel_ns(atomic_load(&r->finish_ns)),
                (i + 1 == task_cnt) ? "" : ",");
    }
    fprintf(f, "  ],\n");

    /* [role, kind, tid, sub, n, start, end] */
    fprintf(f, "  \"phases\": [\n");
    for (uint32_t i = 0; i < phase_cnt; i++) {
        const swim_phase_rec_t *r = &g_swim_phases[i];
        fprintf(f, "    [%u,%u,%u,%u,%u,%llu,%llu]%s\n",
                (unsigned)r->role, (unsigned)r->kind, (unsigned)r->tid,
                (unsigned)r->sub, r->n,
                (unsigned long long)rel_ns(r->start_ns),
                (unsigned long long)rel_ns(r->end_ns),
                (i + 1 == phase_cnt) ? "" : ",");
    }
    fprintf(f, "  ],\n");

    /* [task_id, kind, ns] */
    fprintf(f, "  \"marks\": [\n");
    for (uint32_t i = 0; i < mark_cnt; i++) {
        const swim_mark_rec_t *r = &g_swim_marks[i];
        fprintf(f, "    [%u,%u,%llu]%s\n", r->task_id, r->kind,
                (unsigned long long)rel_ns(r->ns),
                (i + 1 == mark_cnt) ? "" : ",");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"dropped\": {\"tasks\": %u, \"phases\": %u, \"marks\": %u}\n",
            (unsigned)atomic_load(&g_swim_task_drop),
            (unsigned)atomic_load(&g_swim_phase_drop),
            (unsigned)atomic_load(&g_swim_mark_drop));
    fprintf(f, "}\n");
    fclose(f);

    printf("[swimlane] %s: %u tasks, %u phases, %u marks",
           path, task_cnt, phase_cnt, mark_cnt);
    uint32_t dropped = atomic_load(&g_swim_task_drop) +
                       atomic_load(&g_swim_phase_drop) +
                       atomic_load(&g_swim_mark_drop);
    if (dropped > 0) {
        printf(" (dropped %u — raise SWIM_MAX_* in swimlane.h)", dropped);
    }
    printf("\n");

    free(g_swim_scratch);
    free(g_swim_tasks);
    free(g_swim_phases);
    free(g_swim_marks);
    g_swim_scratch = NULL;
    g_swim_tasks = NULL;
    g_swim_phases = NULL;
    g_swim_marks = NULL;
}

#else /* !SWIMLANE */

/* ISO C 不允许空翻译单元，占位即可 */
typedef int swimlane_disabled_translation_unit_t;

#endif /* SWIMLANE */
