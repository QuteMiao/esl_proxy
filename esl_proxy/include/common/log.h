/*
 * log.h - Lightweight logging macros for scheduler and algorithm subsystems
 *
 * Compile-time flags (set via -D or conf.h):
 *   WORKER_LOG  - enable WORKER_LOGF() worker-thread logging macro
 *   MAIN_LOG    - enable MAIN_LOGF() main-thread logging macro
 *
 * Runtime (WORKER_LOG only): g_worker_log toggles output, g_log_output_mode
 * selects destination (0=file, 1=stdout, 2=both). The scheduler build
 * defaults to stdout-only; the algorithm build adds per-thread file output.
 */

#ifndef SCHEDULER_LOG_H
#define SCHEDULER_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#if WORKER_LOG || MAIN_LOG
/* monotonic nanosecond clock — only compiled when logging is enabled */
static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
#endif

/* ── Worker-thread logging ───────────────────────────────────────────── */

#if WORKER_LOG
extern int g_worker_log;
extern int g_log_output_mode;

void log_init(const char *base_filename);
void log_close(void);

/* Internal: forwarded by WORKER_LOGF macro */
void log_write(const char *file, int line, const char *fmt, ...);

/* Variadic dispatch helpers (0–5 args) */
#define _LOG_WRITE_0(file, line, fmt) \
    log_write(file, line, fmt)
#define _LOG_WRITE_1(file, line, fmt, a1) \
    log_write(file, line, fmt, a1)
#define _LOG_WRITE_2(file, line, fmt, a1, a2) \
    log_write(file, line, fmt, a1, a2)
#define _LOG_WRITE_3(file, line, fmt, a1, a2, a3) \
    log_write(file, line, fmt, a1, a2, a3)
#define _LOG_WRITE_4(file, line, fmt, a1, a2, a3, a4) \
    log_write(file, line, fmt, a1, a2, a3, a4)
#define _LOG_WRITE_5(file, line, fmt, a1, a2, a3, a4, a5) \
    log_write(file, line, fmt, a1, a2, a3, a4, a5)
#define _LOG_WRITE_GET(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

/* WORKER_LOGF — worker-thread CSV/log output
 *
 * Output format: "[<file>:<line>] <fmt...>\\n"
 * Thread-safe: protected by an internal mutex inside log_write().
 * Enabled at runtime when g_worker_log != 0. */
#define WORKER_LOGF(...)                                                   \
    do {                                                                    \
        if (g_worker_log) {                                                 \
            _LOG_WRITE_GET(__VA_ARGS__, _LOG_WRITE_5, _LOG_WRITE_4,        \
                           _LOG_WRITE_3, _LOG_WRITE_2, _LOG_WRITE_1,       \
                           _LOG_WRITE_0)(__FILE__, __LINE__, __VA_ARGS__);  \
        }                                                                   \
    } while (0)

#else
#define WORKER_LOGF(...) ((void)0)
#endif /* WORKER_LOG */

/* ── Main-thread logging ─────────────────────────────────────────────── */

#if MAIN_LOG
/* Internal: forwarded by MAIN_LOGF macro */
void main_log_write(int line, const char *fmt, ...);

#define _MAIN_LOG_WRITE_0(line, fmt) main_log_write(line, fmt)
#define _MAIN_LOG_WRITE_1(line, fmt, a1) main_log_write(line, fmt, a1)
#define _MAIN_LOG_WRITE_2(line, fmt, a1, a2) main_log_write(line, fmt, a1, a2)
#define _MAIN_LOG_WRITE_3(line, fmt, a1, a2, a3) main_log_write(line, fmt, a1, a2, a3)
#define _MAIN_LOG_WRITE_4(line, fmt, a1, a2, a3, a4) main_log_write(line, fmt, a1, a2, a3, a4)
#define _MAIN_LOG_WRITE_5(line, fmt, a1, a2, a3, a4, a5) main_log_write(line, fmt, a1, a2, a3, a4, a5)
#define _MAIN_LOG_WRITE_GET(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

/* MAIN_LOGF — main-thread log output
 *
 * Output format: "[scheduler:<line>] <fmt...>\\n"
 * Always writes to stdout. */
#define MAIN_LOGF(...)                                                     \
    do {                                                                    \
        _MAIN_LOG_WRITE_GET(__VA_ARGS__, _MAIN_LOG_WRITE_5,               \
                            _MAIN_LOG_WRITE_4, _MAIN_LOG_WRITE_3,          \
                            _MAIN_LOG_WRITE_2, _MAIN_LOG_WRITE_1,          \
                            _MAIN_LOG_WRITE_0)(__LINE__, __VA_ARGS__);     \
    } while (0)

#else
#define MAIN_LOGF(...) ((void)0)
#endif /* MAIN_LOG */

#endif /* SCHEDULER_LOG_H */
