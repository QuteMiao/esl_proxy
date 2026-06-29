/* onboard_log.c — CANN dlog backend for AICPU onboard (onboard_log.h). */
#define _GNU_SOURCE

#include "onboard_log.h"
#include "tools.h"

#include <stdarg.h>
#include <stdbool.h>

bool g_is_log_enable_debug = false;
bool g_is_log_enable_info = false;
bool g_is_log_enable_warn = false;
bool g_is_log_enable_error = false;
int g_log_info_v = 5;

void init_log_switch(void)
{
    g_is_log_enable_debug = CheckLogLevel(AICPU, DLOG_DEBUG);
    g_is_log_enable_info = CheckLogLevel(AICPU, DLOG_INFO);
    g_is_log_enable_warn = CheckLogLevel(AICPU, DLOG_WARN);
    g_is_log_enable_error = CheckLogLevel(AICPU, DLOG_ERROR);
}

static void dev_vlog_emit(int level, int info_v, const char *func, const char *fmt, va_list args)
{
    char buffer[2048];

    vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (level == DLOG_DEBUG) {
        dlog_debug(AICPU, "%lu %s\n\"%s\"", GET_TID(), func, buffer);
    } else if (level == DLOG_WARN) {
        dlog_warn(AICPU, "%lu %s\n\"%s\"", GET_TID(), func, buffer);
    } else if (level == DLOG_ERROR) {
        dlog_error(AICPU, "%lu %s\n\"%s\"", GET_TID(), func, buffer);
    } else {
        dlog_info(AICPU, "%lu %s [V%d]\n\"%s\"", GET_TID(), func, info_v, buffer);
    }
}

void dev_vlog_debug(const char *func, const char *fmt, va_list args)
{
    dev_vlog_emit(DLOG_DEBUG, 0, func, fmt, args);
}

void dev_vlog_warn(const char *func, const char *fmt, va_list args)
{
    dev_vlog_emit(DLOG_WARN, 0, func, fmt, args);
}

void dev_vlog_error(const char *func, const char *fmt, va_list args)
{
    dev_vlog_emit(DLOG_ERROR, 0, func, fmt, args);
}

void dev_vlog_info_v(int v, const char *func, const char *fmt, va_list args)
{
    dev_vlog_emit(DLOG_INFO, v, func, fmt, args);
}
