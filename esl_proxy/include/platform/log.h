/*
 * log.h — unified logging interface for algorithm and platform code.
 * Sim host: sim/log.h (stdio + optional CSV worker logs).
 * Onboard:  onboard_log.h (CANN dlog).
 */
#ifndef ESL_PROXY_LOG_H
#define ESL_PROXY_LOG_H

#if defined(ESL_PROXY_ONBOARD) || defined(ESL_PROXY_ONBOARD_HOST)
#include "onboard_log.h"

#define MAIN_LOGF(...) ((void)0)
#define WORKER_LOGF(...) ((void)0)

static inline void log_init(const char *base_filename)
{
    (void)base_filename;
}

static inline void log_close(void) {}
#else
#include "sim/log.h"
#endif

#endif /* ESL_PROXY_LOG_H */
