/*
 * log.h - Toggleable worker logging
 *
 * Compile-time: set WORKER_LOG to 1 in conf.h to include log calls.
 * Runtime: set g_worker_log to 1, or export WORKER_LOG=1 in the environment.
 *
 * Log format: tag,source,log_line,detail
 * Each log entry is CSV formatted for easy analysis.
 */

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdint.h>

#include "conf.h"

#if WORKER_LOG
extern int g_worker_log;
extern FILE *g_log_file;
extern uint64_t g_log_line;

#define WORKER_LOGF(tag, fmt, ...)                                           \
    do {                                                                     \
        if (g_worker_log && g_log_file) {                                    \
            fprintf(g_log_file, "%s,%s:%d,%llu," fmt "\n",                  \
                    tag,                                                    \
                    __FILE__, __LINE__,                                     \
                    (unsigned long long)g_log_line                          \
                    __VA_OPT__(,) __VA_ARGS__);                              \
            g_log_line++;                                                    \
        }                                                                    \
    } while (0)
#else
#define WORKER_LOGF(tag, fmt, ...) ((void)0)
#endif

void log_init(const char *filename);
void log_close(void);

#endif /* LOG_H */
