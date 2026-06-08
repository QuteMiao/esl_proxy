/*
 * test_dep_dump.c - smoke test for post-orchestration DAG export
 */

#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DAG_RING_BUF_H

#include "conf.h"
#include "task.h"

atomic_int g_task_id = 0;
uint16_t g_min_uncomplete_task = 0;
_Atomic task_state g_state_buf[RING_SIZE];
_Atomic uint16_t g_predecessor_buf[RING_SIZE];
struct task_desc g_basic_buf[RING_SIZE];
struct succ_list g_successor_buf[RING_SIZE];
struct succ_list g_successor_exp_buf[HALF_RING_SIZE];

static void ring_buf_init_local(void)
{
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_successor_buf[i].next = &g_successor_exp_buf[i % HALF_RING_SIZE];
    }
    for (size_t i = 0; i < HALF_RING_SIZE; i++) {
        g_successor_exp_buf[i].next = NULL;
    }
}

static bool succeed_local(uint16_t consumer, uint16_t producer)
{
    const int slot = producer & RING_MASK;
    task_state expected =
        atomic_load_explicit(&g_state_buf[slot], memory_order_relaxed);
    expected.state = TASK_STATUS_CREATING;

    task_state desired = expected;
    desired.successor_cnt = expected.successor_cnt + 1;
    if (!atomic_compare_exchange_strong(&g_state_buf[slot], &expected,
                                        desired))
        return false;

    uint32_t idx = expected.successor_cnt;
    struct succ_list *ptr = &g_successor_buf[slot];
    while (idx >= (uint32_t)SUCC_NODE_CNT) {
        idx -= (uint32_t)SUCC_NODE_CNT;
        ptr = ptr->next;
    }
    ptr->successor[idx] = consumer;
    atomic_fetch_add_explicit(&g_predecessor_buf[consumer & RING_MASK], 1,
                              memory_order_relaxed);
    return true;
}

static void init_task(uint16_t id)
{
    const int slot = id & RING_MASK;
    memset(&g_basic_buf[slot], 0, sizeof g_basic_buf[slot]);
    g_basic_buf[slot].id = id;
    g_basic_buf[slot].type = TASK_TYPE_VECTOR;
    g_basic_buf[slot].mode = ORG_MODE_SINGLE;
    g_basic_buf[slot].duration = (uint16_t)(1000u + id);

    task_state st = {.state = TASK_STATUS_CREATING,
                     .task_id = id,
                     .successor_cnt = 0};
    atomic_store_explicit(&g_state_buf[slot], st, memory_order_relaxed);
    atomic_store_explicit(&g_predecessor_buf[slot], 0, memory_order_relaxed);
}

#include "dep_dump.h"

int main(void)
{
    ring_buf_init_local();

    /* Small DAG: 1->{2,3}, 2->{4} => 3 edges */
    g_task_id = 4;
    for (uint16_t id = 1; id <= 4; id++)
        init_task(id);
    assert(succeed_local(2, 1));
    assert(succeed_local(3, 1));
    assert(succeed_local(4, 2));
    assert(dep_dump_count_edges() == 3);

    /* Chain >64 successors on one producer (tests succ_list next traversal). */
    ring_buf_init_local();
    g_task_id = 66;
    for (uint16_t id = 1; id <= 66; id++)
        init_task(id);
    for (uint16_t c = 2; c <= 66; c++)
        assert(succeed_local(c, 1));
    assert(dep_dump_count_edges() == 65);

    dep_dump_summary(stdout);
    printf("test_dep_dump: OK\n");
    return 0;
}
