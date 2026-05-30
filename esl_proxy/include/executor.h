/*
 * executor.h - Executor Type Definition
 *
 * Defines the executor type used by Dispatch for task execution.
 * The executor runs tasks in its 2-slot PING PONG cache.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef DAG_EXECUTOR_H
#define DAG_EXECUTOR_H

#include <stdint.h>
#include <stddef.h>
#include "conf.h"

/*
 * Executor
 * Contains 2 slots for task caching with PING PONG selection
 */
typedef struct executor {
    uint16_t tasks[AIC_OSTD];
    uint16_t index[AIC_OSTD];
    uint64_t base[AIC_OSTD];
} executor_t;

#endif /* DAG_EXECUTOR_H */