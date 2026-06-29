/*
 * sim/log.h — sim host logging backend (stdio + optional CSV worker logs).
 *
 * Compile-time:
 *   WORKER_LOG — worker thread CSV logs (conf.h)
 *   MAIN_LOG   — main thread screen logs (conf.h)
 *
 * LOG_ERROR / LOG_INFO_V0 — always-on milestone and error traces (stdio).
 */
#ifndef ESL_PROXY_SIM_LOG_H
#define ESL_PROXY_SIM_LOG_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#define LOG_MAX_THREADS 16

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "conf.h"

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#if WORKER_LOG
extern int g_worker_log;
extern int g_log_output_mode;

void log_init(const char *base_filename);
void log_close(void);
void log_write(const char *file, int line, const char *fmt, ...);

#define _LOG_WRITE_0(file, line, fmt) log_write(file, line, fmt)
#define _LOG_WRITE_1(file, line, fmt, arg1) log_write(file, line, fmt, arg1)
#define _LOG_WRITE_2(file, line, fmt, arg1, arg2) log_write(file, line, fmt, arg1, arg2)
#define _LOG_WRITE_3(file, line, fmt, arg1, arg2, arg3) log_write(file, line, fmt, arg1, arg2, arg3)
#define _LOG_WRITE_4(file, line, fmt, arg1, arg2, arg3, arg4) log_write(file, line, fmt, arg1, arg2, arg3, arg4)
#define _LOG_WRITE_5(file, line, fmt, arg1, arg2, arg3, arg4, arg5) \
    log_write(file, line, fmt, arg1, arg2, arg3, arg4, arg5)
#define _LOG_WRITE_GET(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

#define WORKER_LOGF(...)                                                  \
    do {                                                                  \
        if (g_worker_log) {                                               \
            _LOG_WRITE_GET(__VA_ARGS__, _LOG_WRITE_5, _LOG_WRITE_4,       \
                           _LOG_WRITE_3, _LOG_WRITE_2, _LOG_WRITE_1,      \
                           _LOG_WRITE_0)(__FILE__, __LINE__, __VA_ARGS__); \
        }                                                                 \
    } while (0)

#else
#define WORKER_LOGF(...) ((void)0)
static inline void log_init(const char *base_filename)
{
    (void)base_filename;
}
static inline void log_close(void) {}
#endif

#if MAIN_LOG
void main_log_write(int line, const char *fmt, ...);

#define _MAIN_LOG_WRITE_0(line, fmt) main_log_write(line, fmt)
#define _MAIN_LOG_WRITE_1(line, fmt, arg1) main_log_write(line, fmt, arg1)
#define _MAIN_LOG_WRITE_2(line, fmt, arg1, arg2) main_log_write(line, fmt, arg1, arg2)
#define _MAIN_LOG_WRITE_3(line, fmt, arg1, arg2, arg3) main_log_write(line, fmt, arg1, arg2, arg3)
#define _MAIN_LOG_WRITE_4(line, fmt, arg1, arg2, arg3, arg4) \
    main_log_write(line, fmt, arg1, arg2, arg3, arg4)
#define _MAIN_LOG_WRITE_5(line, fmt, arg1, arg2, arg3, arg4, arg5) \
    main_log_write(line, fmt, arg1, arg2, arg3, arg4, arg5)
#define _MAIN_LOG_WRITE_GET(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

#define MAIN_LOGF(...)                                                     \
    do {                                                                   \
        _MAIN_LOG_WRITE_GET(__VA_ARGS__, _MAIN_LOG_WRITE_5, _MAIN_LOG_WRITE_4, \
                            _MAIN_LOG_WRITE_3, _MAIN_LOG_WRITE_2, _MAIN_LOG_WRITE_1, \
                            _MAIN_LOG_WRITE_0)(__LINE__, __VA_ARGS__);     \
    } while (0)

#else
#define MAIN_LOGF(...) ((void)0)
#endif

#define __SIM_FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define LOG_ERROR(fmt, ...) \
    do { \
        (void)fprintf(stderr, "[%s:%d] " fmt "\n", __SIM_FILENAME__, __LINE__, ##__VA_ARGS__); \
    } while (0)

#define LOG_INFO_V0(fmt, ...) \
    do { \
        (void)fprintf(stdout, "[%s:%d] " fmt "\n", __SIM_FILENAME__, __LINE__, ##__VA_ARGS__); \
    } while (0)

#endif /* ESL_PROXY_SIM_LOG_H */
