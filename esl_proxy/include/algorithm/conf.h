#ifndef CONF_H
#define CONF_H

#include "worker_map.h"

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 2048
#define NODE_BUFF_SIZE 65536

#define AIC_OSTD 2
#define AIC_CNT ESL_PROXY_WORKER_BLOCK_DIM
#define EXE_TYPE_CNT 2
#define CON_NODE_CNT 256

#define CUTTER_BATCH_SIZE 512
#define ADD_BATCH_SIZE 240
#define LOCAL_BUFFER_SIZE 512
#define DISPATCH_COMPLETE_BATCH 512

/* Number of scheduler lanes. Each lane = one cutter thread paired 1:1 with one
 * dispatch thread and a disjoint, strided subset of AICore cores
 * (lane i owns cores { c : c % ESL_LANE_CNT == i }). Set this to scale. */
#ifndef ESL_LANE_CNT
#define ESL_LANE_CNT 1
#endif

/* Overlapped model only: the orchestrator runs as a standalone thread,
 * overlapping with cutter/dispatch. The folded (orchestrator-first) model
 * is not included in this branch. */
#define ESL_ORCH_FIRST 0

#define CUTTER_THREAD_CNT ESL_LANE_CNT
#define DISPATCH_THREAD_CNT ESL_LANE_CNT
#define EXECUTOR_THREAD_CNT 1

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(CUTTER_THREAD_CNT == DISPATCH_THREAD_CNT,
               "cutter and dispatch threads must be paired 1:1");
#endif

/* core -> owning lane (strided). Matches executor.c completion routing. */
#define CORE_LANE(core) ((core) % DISPATCH_THREAD_CNT)

/* Dispatch mode. 0 (default) = basic dispatch; 1 = double-buffer dispatch.
 * Selected via `make DISPATCH=double_buffer` (Makefile adds -D=1). Basic mode
 * filters cores held by in-flight MIX clusters (dispatch_mix_core_busy) and
 * runs a second-pass MIX prefetch (dispatch_mix_prefetch); double-buffer mode
 * skips both so dispatch_prefetch can actively use the 2nd slot. */
#ifndef ESL_DISPATCH_DOUBLE_BUFFER
#define ESL_DISPATCH_DOUBLE_BUFFER 0
#endif

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

#endif /* CONF_H */
