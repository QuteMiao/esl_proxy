#ifndef CONF_H
#define CONF_H

#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 2048

#define SUCC_NODE_CNT 64

#define AIC_OSTD 2
#define AIC_CNT 60
#define EXE_TYPE_CNT 2

#define CUTTER_BATCH_SIZE 64

#define CUTTER_THREAD_CNT 1
#define DISPATCH_THREAD_CNT 1
#define EXECUTOR_THREAD_CNT 1

/* 1: compile in worker logs; toggle at runtime via g_worker_log or WORKER_LOG env */
#define WORKER_LOG 1

/* 1: compile in main thread logs; output to screen only */
#define MAIN_LOG 1

/* Log output mode: 0=file, 1=stdout, 2=both */
#define LOG_OUTPUT_MODE 2

/* 1: enable aicpu_orchestration_entry execution time logging in nanoseconds */
#define ORCHESTRATION_TIME 1

#endif /* CONF_H */
