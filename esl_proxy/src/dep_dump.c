/*
 * dep_dump.c - Post-orchestration static DAG export
 */

#define _GNU_SOURCE

#include "dep_dump.h"

#if DEP_DUMP

#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "conf.h"
#include "ring_buf.h"
#include "task.h"

static const char *task_type_name(task_type_t t)
{
    switch (t) {
    case TASK_TYPE_CUBE: return "CUBE";
    case TASK_TYPE_VECTOR: return "VECTOR";
    case TASK_TYPE_MIX: return "MIX";
    default: return "UNKNOWN";
    }
}

static const char *org_mode_name(org_mode_t m)
{
    switch (m) {
    case ORG_MODE_SINGLE: return "SINGLE";
    case ORG_MODE_GROUP: return "GROUP";
    case ORG_MODE_SPMD_SYNC: return "SPMD_SYNC";
    case ORG_MODE_SPMD_ASYNC: return "SPMD_ASYNC";
    default: return "UNKNOWN";
    }
}

static uint16_t successor_at(const struct succ_list *head, uint32_t index)
{
    uint32_t idx = index;
    const struct succ_list *ptr = head;
    while (idx >= (uint32_t)SUCC_NODE_CNT) {
        idx -= (uint32_t)SUCC_NODE_CNT;
        ptr = ptr->next;
    }
    return ptr->successor[idx];
}

static uint32_t task_successor_cnt(int slot)
{
    const task_state st =
        atomic_load_explicit(&g_state_buf[slot], memory_order_relaxed);
    return st.successor_cnt;
}

static void foreach_edge(void (*fn)(uint16_t producer, uint16_t consumer, void *ctx),
                         void *ctx)
{
    const int last = atomic_load_explicit(&g_task_id, memory_order_relaxed);
    for (int p = 1; p <= last; p++) {
        const int slot = p & RING_MASK;
        const uint32_t cnt = task_successor_cnt(slot);
        for (uint32_t i = 0; i < cnt; i++) {
            const uint16_t c = successor_at(&g_successor_buf[slot], i);
            fn((uint16_t)p, c, ctx);
        }
    }
}

typedef struct {
    uint32_t edges;
    uint32_t max_pred;
    uint32_t max_succ;
} dep_dump_stats_t;

static void count_edge(uint16_t producer, uint16_t consumer, void *ctx)
{
    (void)producer;
    (void)consumer;
    dep_dump_stats_t *st = (dep_dump_stats_t *)ctx;
    st->edges++;
}

uint32_t dep_dump_count_edges(void)
{
    dep_dump_stats_t st = {0, 0, 0};
    foreach_edge(count_edge, &st);
    return st.edges;
}

void dep_dump_edges_csv(FILE *out)
{
    fprintf(out, "producer,consumer\n");
    for (int p = 1; p <= atomic_load_explicit(&g_task_id, memory_order_relaxed);
         p++) {
        const int slot = p & RING_MASK;
        const uint32_t cnt = task_successor_cnt(slot);
        for (uint32_t i = 0; i < cnt; i++) {
            const uint16_t c = successor_at(&g_successor_buf[slot], i);
            fprintf(out, "%d,%u\n", p, (unsigned)c);
        }
    }
}

void dep_dump_tasks_csv(FILE *out)
{
    fprintf(out, "task_id,type,mode,count,duration,pred_cnt,succ_cnt\n");
    const int last = atomic_load_explicit(&g_task_id, memory_order_relaxed);
    for (int t = 1; t <= last; t++) {
        const int slot = t & RING_MASK;
        const struct task_desc *d = &g_basic_buf[slot];
        const uint16_t pred =
            atomic_load_explicit(&g_predecessor_buf[slot], memory_order_relaxed);
        const uint32_t succ = task_successor_cnt(slot);
        fprintf(out, "%d,%s,%s,%u,%u,%u,%u\n", t, task_type_name(d->type),
                org_mode_name(d->mode), d->count, d->duration, (unsigned)pred,
                (unsigned)succ);
    }
}

void dep_dump_dot(FILE *out)
{
    fprintf(out, "digraph G {\n");
    const int last = atomic_load_explicit(&g_task_id, memory_order_relaxed);
    for (int t = 1; t <= last; t++) {
        const int slot = t & RING_MASK;
        const struct task_desc *d = &g_basic_buf[slot];
        fprintf(out, "  t%d [label=\"t%d\\n%s %s\\ndur=%u cnt=%u\"];\n", t, t,
                task_type_name(d->type), org_mode_name(d->mode), d->duration,
                d->count);
    }
    for (int p = 1; p <= last; p++) {
        const int slot = p & RING_MASK;
        const uint32_t cnt = task_successor_cnt(slot);
        for (uint32_t i = 0; i < cnt; i++) {
            const uint16_t c = successor_at(&g_successor_buf[slot], i);
            fprintf(out, "  t%d -> t%u;\n", p, (unsigned)c);
        }
    }
    fprintf(out, "}\n");
}

void dep_dump_summary(FILE *out)
{
    dep_dump_stats_t st = {0, 0, 0};
    foreach_edge(count_edge, &st);

    const int last = atomic_load_explicit(&g_task_id, memory_order_relaxed);
    for (int t = 1; t <= last; t++) {
        const int slot = t & RING_MASK;
        const uint16_t pred =
            atomic_load_explicit(&g_predecessor_buf[slot], memory_order_relaxed);
        const uint32_t succ = task_successor_cnt(slot);
        if ((uint32_t)pred > st.max_pred)
            st.max_pred = pred;
        if (succ > st.max_succ)
            st.max_succ = succ;
    }

    fprintf(out, "tasks=%d edges=%u max_pred=%u max_succ=%u\n", last,
            st.edges, st.max_pred, st.max_succ);
}

static void dep_dump_print_abspath(const char *path)
{
    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        fprintf(stdout, "[dep_dump] %s\n", resolved);
        fflush(stdout);
        return;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) != NULL)
        fprintf(stdout, "[dep_dump] %s/%s\n", cwd, path);
    else
        fprintf(stdout, "[dep_dump] %s\n", path);
    fflush(stdout);
}

static int dep_dump_write_file(const char *path, void (*writer)(FILE *))
{
    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "[dep_dump] failed to open %s\n", path);
        return -1;
    }
    writer(fp);
    fclose(fp);
    dep_dump_print_abspath(path);
    return 0;
}

static void dep_dump_resolve_prefix(const char *prefix, char *out, size_t outsz)
{
    if (prefix[0] == '/') {
        snprintf(out, outsz, "%s", prefix);
        return;
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd) != NULL)
        snprintf(out, outsz, "%s/%s", cwd, prefix);
    else
        snprintf(out, outsz, "%s", prefix);
}

static int format_has(const char *formats, const char *token)
{
    const char *p = formats;
    const size_t n = strlen(token);
    if (n == 0)
        return 0;
    while (*p) {
        if (strncmp(p, token, n) == 0 && (p[n] == '\0' || p[n] == ','))
            return 1;
        const char *comma = strchr(p, ',');
        if (!comma)
            break;
        p = comma + 1;
    }
    return 0;
}

void dep_dump_maybe(void)
{
    const char *enabled = getenv("DEP_DUMP");
    if (enabled == NULL || enabled[0] != '1')
        return;

    const char *prefix = getenv("DEP_DUMP_FILE");
    if (prefix == NULL || prefix[0] == '\0')
        prefix = "build/dep_";

    const char *formats = getenv("DEP_DUMP_FORMAT");
    if (formats == NULL || formats[0] == '\0')
        formats = "summary,csv,dot";

    char path[512];
    char prefix_abs[PATH_MAX];
    dep_dump_resolve_prefix(prefix, prefix_abs, sizeof prefix_abs);
    fprintf(stdout, "[dep_dump] output prefix: %s\n", prefix_abs);
    fflush(stdout);

    if (format_has(formats, "summary")) {
        fprintf(stdout, "[dep_dump] summary -> stdout\n");
        fflush(stdout);
        dep_dump_summary(stdout);
        fflush(stdout);
    }

    if (format_has(formats, "csv")) {
        snprintf(path, sizeof path, "%stasks.csv", prefix);
        dep_dump_write_file(path, dep_dump_tasks_csv);
        snprintf(path, sizeof path, "%sedges.csv", prefix);
        dep_dump_write_file(path, dep_dump_edges_csv);
    }

    if (format_has(formats, "dot")) {
        snprintf(path, sizeof path, "%sdag.dot", prefix);
        dep_dump_write_file(path, dep_dump_dot);
    }
}

#endif /* DEP_DUMP */
