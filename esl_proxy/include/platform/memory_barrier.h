/*
 * Onboard memory barriers for AICPU / shared-GM protocols.
 * aarch64: hardware DMB/DSB; other arches: compiler barrier only.
 */
#ifndef ESL_PROXY_ONBOARD_MEMORY_BARRIER_H
#define ESL_PROXY_ONBOARD_MEMORY_BARRIER_H

#ifdef __aarch64__
#define ESL_RMB() __asm__ __volatile__("dsb ld" ::: "memory")
#define ESL_WMB() __asm__ __volatile__("dsb st" ::: "memory")
#define OUT_OF_ORDER_STORE_BARRIER() __asm__ __volatile__("dmb ishst" ::: "memory")
#define OUT_OF_ORDER_LOAD_BARRIER() __asm__ __volatile__("dmb ishld" ::: "memory")
#else
#define ESL_RMB() __asm__ __volatile__("" ::: "memory")
#define ESL_WMB() __asm__ __volatile__("" ::: "memory")
#define OUT_OF_ORDER_STORE_BARRIER() __asm__ __volatile__("" ::: "memory")
#define OUT_OF_ORDER_LOAD_BARRIER() __asm__ __volatile__("" ::: "memory")
#endif

/* Legacy swimlane aliases */
#define rmb() ESL_RMB()
#define wmb() ESL_WMB()

#endif /* ESL_PROXY_ONBOARD_MEMORY_BARRIER_H */
