/*
 * AICPU onboard logging — CANN dlog (levels aligned with log_types.h / dlog_pub.h).
 *
 * Mapping:
 *   LOG_ERROR     -> DLOG_ERROR
 *   LOG_WARN      -> DLOG_WARN
 *   LOG_DEBUG     -> DLOG_DEBUG
 *   LOG_INFO_V0   -> DLOG_DEBUG  (summary / milestone traces)
 *   LOG_INFO_V1-9 -> DLOG_INFO    (filtered by g_log_info_v)
 */
#ifndef ESL_PROXY_ONBOARD_LOG_H
#define ESL_PROXY_ONBOARD_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dlog_pub.h"
#include "log_types.h"

extern bool g_is_log_enable_debug;
extern bool g_is_log_enable_info;
extern bool g_is_log_enable_warn;
extern bool g_is_log_enable_error;
extern int g_log_info_v;

#ifdef __cplusplus
extern "C" {
#endif

static inline void set_log_level(int level)
{
    (void)level;
}

static inline void set_log_info_v(int v)
{
    if (v < 0) {
        v = 0;
    }
    if (v > 9) {
        v = 9;
    }
    g_log_info_v = v;
}

static inline int get_log_info_v(void)
{
    return g_log_info_v;
}

void init_log_switch(void);

void dev_vlog_debug(const char *func, const char *fmt, va_list args);
void dev_vlog_warn(const char *func, const char *fmt, va_list args);
void dev_vlog_error(const char *func, const char *fmt, va_list args);
void dev_vlog_info_v(int v, const char *func, const char *fmt, va_list args);

#ifdef __cplusplus
}
#endif

static inline bool is_log_enable_debug(void) { return g_is_log_enable_debug; }
static inline bool is_log_enable_info(void) { return g_is_log_enable_info; }
static inline bool is_log_enable_warn(void) { return g_is_log_enable_warn; }
static inline bool is_log_enable_error(void) { return g_is_log_enable_error; }

static inline void unified_log_error(const char *func, const char *fmt, ...)
{
    va_list args;

    if (!is_log_enable_error()) {
        return;
    }
    va_start(args, fmt);
    dev_vlog_error(func, fmt, args);
    va_end(args);
}

static inline void unified_log_warn(const char *func, const char *fmt, ...)
{
    va_list args;

    if (!is_log_enable_warn()) {
        return;
    }
    va_start(args, fmt);
    dev_vlog_warn(func, fmt, args);
    va_end(args);
}

static inline void unified_log_debug(const char *func, const char *fmt, ...)
{
    va_list args;

    if (!is_log_enable_debug()) {
        return;
    }
    va_start(args, fmt);
    dev_vlog_debug(func, fmt, args);
    va_end(args);
}

static inline void unified_log_info_v(const char *func, int v, const char *fmt, ...)
{
    va_list args;

    if (!is_log_enable_info() || v < g_log_info_v) {
        return;
    }
    va_start(args, fmt);
    dev_vlog_info_v(v, func, fmt, args);
    va_end(args);
}

#define __ONBOARD_FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define LOG_ERROR(fmt, ...) unified_log_error(__FUNCTION__, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) unified_log_warn(__FUNCTION__, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) unified_log_debug(__FUNCTION__, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V0(fmt, ...) unified_log_debug(__FUNCTION__, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V1(fmt, ...) unified_log_info_v(__FUNCTION__, 1, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V2(fmt, ...) unified_log_info_v(__FUNCTION__, 2, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V3(fmt, ...) unified_log_info_v(__FUNCTION__, 3, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V4(fmt, ...) unified_log_info_v(__FUNCTION__, 4, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V5(fmt, ...) unified_log_info_v(__FUNCTION__, 5, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V6(fmt, ...) unified_log_info_v(__FUNCTION__, 6, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V7(fmt, ...) unified_log_info_v(__FUNCTION__, 7, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V8(fmt, ...) unified_log_info_v(__FUNCTION__, 8, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO_V9(fmt, ...) unified_log_info_v(__FUNCTION__, 9, "[%s:%d] " fmt, __ONBOARD_FILENAME__, __LINE__, ##__VA_ARGS__)

/* Lightweight direct dlog for bootstrap-only TUs (e.g. aicpu_dispatcher). */
static inline void onboard_dispatcher_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dlog_error(CCECPU, "[esl-dispatcher] %s", buf);
}

#endif /* ESL_PROXY_ONBOARD_LOG_H */
