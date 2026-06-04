/*
 * log.h - Toggleable worker logging
 *
 * Compile-time: 
 *   - WORKER_LOG: set to 1 in conf.h to include worker log calls (file output)
 *   - MAIN_LOG: set to 1 in conf.h to include main thread log calls (screen output)
 * Runtime (WORKER_LOG only): set g_worker_log to 1, or export WORKER_LOG=1 in the environment.
 *
 * Log format: source,log_line,detail
 * Each log entry is CSV formatted for easy analysis.
 *
 * Thread-safe: worker logs are split into separate files per thread.
 * Each thread writes to its own file: <base_filename>_thread_<tid>.csv
 */

#ifndef LOG_H
#define LOG_H

#define LOG_MAX_THREADS 64

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>

#include "conf.h"

#if WORKER_LOG
extern int g_worker_log;
extern int g_log_output_mode;

void log_init(const char *base_filename);
void log_close(void);

// Internal function called by WORKER_LOGF macro
void log_write(const char *file, int line, const char *fmt, ...);

// Variadic macro helper - count arguments and dispatch to log_write
#define _LOG_WRITE_0(file, line, fmt) \
    log_write(file, line, fmt)
#define _LOG_WRITE_1(file, line, fmt, arg1) \
    log_write(file, line, fmt, arg1)
#define _LOG_WRITE_2(file, line, fmt, arg1, arg2) \
    log_write(file, line, fmt, arg1, arg2)
#define _LOG_WRITE_3(file, line, fmt, arg1, arg2, arg3) \
    log_write(file, line, fmt, arg1, arg2, arg3)
#define _LOG_WRITE_4(file, line, fmt, arg1, arg2, arg3, arg4) \
    log_write(file, line, fmt, arg1, arg2, arg3, arg4)
#define _LOG_WRITE_5(file, line, fmt, arg1, arg2, arg3, arg4, arg5) \
    log_write(file, line, fmt, arg1, arg2, arg3, arg4, arg5)
#define _LOG_WRITE_GET(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

// WORKER_LOGF - worker thread logging macro
// Uses log_write() which outputs to pto._thread_<id>.csv
#define WORKER_LOGF(...)                                                  \
    do {                                                                   \
        if (g_worker_log) {                                                \
            _LOG_WRITE_GET(__VA_ARGS__, _LOG_WRITE_5, _LOG_WRITE_4,       \
                           _LOG_WRITE_3, _LOG_WRITE_2, _LOG_WRITE_1,       \
                           _LOG_WRITE_0)(__FILE__, __LINE__, __VA_ARGS__); \
        }                                                                  \
    } while (0)

#else
#define WORKER_LOGF(...) ((void)0)
#endif

#if MAIN_LOG
// Internal function called by MAIN_LOGF macro
void main_log_write(int line, const char *fmt, ...);

// Main thread logging function - count arguments and dispatch
#define _MAIN_LOG_WRITE_0(line, fmt) main_log_write(line, fmt)
#define _MAIN_LOG_WRITE_1(line, fmt, arg1) main_log_write(line, fmt, arg1)
#define _MAIN_LOG_WRITE_2(line, fmt, arg1, arg2) main_log_write(line, fmt, arg1, arg2)
#define _MAIN_LOG_WRITE_3(line, fmt, arg1, arg2, arg3) main_log_write(line, fmt, arg1, arg2, arg3)
#define _MAIN_LOG_WRITE_4(line, fmt, arg1, arg2, arg3, arg4) main_log_write(line, fmt, arg1, arg2, arg3, arg4)
#define _MAIN_LOG_WRITE_5(line, fmt, arg1, arg2, arg3, arg4, arg5) main_log_write(line, fmt, arg1, arg2, arg3, arg4, arg5)
#define _MAIN_LOG_WRITE_GET(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

// MAIN_LOGF - main thread logging macro
// Uses main_log_write() which outputs to screen only
#define MAIN_LOGF(...)                                                     \
    do {                                                                   \
        _MAIN_LOG_WRITE_GET(__VA_ARGS__, _MAIN_LOG_WRITE_5,               \
                            _MAIN_LOG_WRITE_4, _MAIN_LOG_WRITE_3,           \
                            _MAIN_LOG_WRITE_2, _MAIN_LOG_WRITE_1,          \
                            _MAIN_LOG_WRITE_0)(__LINE__, __VA_ARGS__);     \
    } while (0)

#else
#define MAIN_LOGF(...) ((void)0)
#endif

#endif /* LOG_H */