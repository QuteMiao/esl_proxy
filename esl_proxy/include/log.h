/*
 * log.h - Toggleable worker logging
 *
 * Compile-time: set WORKER_LOG to 1 in conf.h to include log calls.
 * Runtime: set g_worker_log to 1, or export WORKER_LOG=1 in the environment.
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#include "conf.h"

#if WORKER_LOG
extern int g_worker_log;

#define WORKER_LOGF(tag, fmt, ...)                                           \
    do {                                                                     \
        if (g_worker_log)                                                    \
            fprintf(stderr, "[%s] " fmt "\n", (tag), ##__VA_ARGS__);         \
    } while (0)
#else
#define WORKER_LOGF(tag, fmt, ...) ((void)0)
#endif

#endif /* LOG_H */
