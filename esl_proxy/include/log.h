/*
 * log.h - Toggleable worker logging
 *
 * Compile-time: set WORKER_LOG to 1 in conf.h to include log calls.
 * Runtime: set g_worker_log to 1, or export WORKER_LOG=1 in the environment.
 *
 * Log format: source,log_line,detail
 * Each log entry is CSV formatted for easy analysis.
 *
 * Thread-safe: logs are split into separate files per thread.
 * Each thread writes to its own file: <base_filename>_thread_<tid>.csv
 */

#ifndef LOG_H
#define LOG_H

#define LOG_MAX_THREADS 64

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#include "conf.h"

#if WORKER_LOG
extern int g_worker_log;
extern int g_log_output_mode;

// Variadic macro helper - dispatch to log_write
#define _LOG_WRITE_1(file, line, fmt) \
    log_write(file, line, fmt)
#define _LOG_WRITE_2(file, line, fmt, arg1) \
    log_write(file, line, fmt, arg1)
#define _LOG_WRITE_3(file, line, fmt, arg1, arg2) \
    log_write(file, line, fmt, arg1, arg2)
#define _LOG_WRITE_4(file, line, fmt, arg1, arg2, arg3) \
    log_write(file, line, fmt, arg1, arg2, arg3)
#define _LOG_WRITE_5(file, line, fmt, arg1, arg2, arg3, arg4) \
    log_write(file, line, fmt, arg1, arg2, arg3, arg4)
#define _LOG_WRITE_GET(_1, _2, _3, _4, _5, NAME, ...) NAME

#define WORKER_LOGF(fmt, ...)                                               \
    do {                                                                     \
        if (g_worker_log) {                                                  \
            _LOG_WRITE_GET(_, ## __VA_ARGS__, _LOG_WRITE_5, _LOG_WRITE_4,    \
                          _LOG_WRITE_3, _LOG_WRITE_2, _LOG_WRITE_1)          \
                (__FILE__, __LINE__, fmt, ## __VA_ARGS__);                   \
        }                                                                    \
    } while (0)
#else
#define WORKER_LOGF(fmt, ...) ((void)0)
#endif

void log_init(const char *base_filename);
void log_close(void);

// Internal function called by WORKER_LOGF macro
void log_write(const char *file, int line, const char *fmt, ...);

#endif /* LOG_H */
