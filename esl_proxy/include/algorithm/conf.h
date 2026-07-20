#ifndef ALGORITHM_CONF_H
#define ALGORITHM_CONF_H

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 2048
/* QWEN3_SPMD_TIER=0：NODE_BUFF_SIZE=65536、CON_NODE_CNT=256。
 * 其他样例沿用修改前默认值（下方）。a2a3 上板在 Makefile 里 -DAIC_CNT=24 覆盖。 */
#define NODE_BUFF_SIZE 8192

// TODO: ERROR
#define CON_NODE_CNT 32

#define AIC_OSTD 2
#ifndef AIC_CNT
#define AIC_CNT 60
#endif
#define EXE_TYPE_CNT 2

#define CQ_BATCH_SIZE 512
#define PRE_BATCH_SIZE 240
#define RQ_BATCH_SIZE 512
#define DISPATCH_COMPLETE_BATCH 512

#define CUTTER_THREAD_CNT 1
#define DISPATCH_THREAD_CNT 1
#define EXECUTOR_THREAD_CNT 1

/* 1: compile in worker logs; toggle at runtime via g_worker_log or WORKER_LOG env */
#ifndef WORKER_LOG
#define WORKER_LOG 1
#endif

/* 1: compile in main thread logs; output to screen only */
#ifndef MAIN_LOG
#define MAIN_LOG 1
#endif

/* Log output mode: 0=file, 1=stdout, 2=both */
#define LOG_OUTPUT_MODE 2

/* 1: enable aicpu_orchestration_entry execution time logging in nanoseconds */
#define ORCHESTRATION_TIME 1

/* 1: compile post-orchestration DAG dump; runtime via DEP_DUMP=1 env */
#ifndef DEP_DUMP
#define DEP_DUMP 0
#endif

/* 1: skip tensormap lookup/insert and succeed(); all tasks submit with no edges */
#ifndef NO_DEPS
#define NO_DEPS 0
#endif

#endif /* ALGORITHM_CONF_H */
