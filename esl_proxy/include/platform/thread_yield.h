/*
 * thread_yield.h — cooperative yield hint for spin-wait loops.
 * Sim host pthread workers: sched_yield(); onboard AICPU: no-op.
 */
#ifndef ESL_PROXY_THREAD_YIELD_H
#define ESL_PROXY_THREAD_YIELD_H

#if defined(ESL_PROXY_ONBOARD) || defined(ESL_PROXY_ONBOARD_HOST)
#define ESL_PLATFORM_SCHED_YIELD() ((void)0)
#else
#include <sched.h>
#define ESL_PLATFORM_SCHED_YIELD() sched_yield()
#endif

#endif /* ESL_PROXY_THREAD_YIELD_H */
