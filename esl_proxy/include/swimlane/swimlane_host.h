/*
 * L2 swimlane host API + onboard runner hooks (host_onboard.c / esl_swimlane_host.cpp).
 */
#ifndef ESL_PROXY_SWIMLANE_HOST_H
#define ESL_PROXY_SWIMLANE_HOST_H

#include <stdint.h>

#ifndef ESL_PROXY_ONBOARD_CONFIG_H
#include "onboard_config.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

void esl_swimlane_host_set_level(int level);
int esl_swimlane_host_init(int worker_count, int aicpu_thread_num, int device_id, const char *output_prefix);
void esl_swimlane_host_start(void);
void esl_swimlane_host_stop_and_export(void);
void esl_swimlane_host_finalize(void);
uint64_t esl_swimlane_host_data_base(void);
uint64_t esl_swimlane_host_rotation_table(void);
void esl_swimlane_host_set_core_types(const int32_t *core_types, int count);

#if ESL_PROXY_ENABLE_L2_SWIMLANE
int esl_swimlane_host_onboard_begin(int device_id, const char *output_prefix);
void esl_swimlane_host_onboard_fill_kargs(void *k_args);
void esl_swimlane_host_onboard_sync_core_types(void *dev_runtime_dev_ptr);
void esl_swimlane_host_onboard_end(void);
#endif

#ifdef __cplusplus
}
#endif

#if ESL_PROXY_ENABLE_L2_SWIMLANE

#define ESL_SWIMLANE_HOST_ONBOARD_BEGIN(device_id, prefix) \
    esl_swimlane_host_onboard_begin((device_id), (prefix))
#define ESL_SWIMLANE_HOST_ONBOARD_FILL_KARGS(k_args_ptr) \
    esl_swimlane_host_onboard_fill_kargs((k_args_ptr))
#define ESL_SWIMLANE_HOST_ONBOARD_SYNC_CORE_TYPES(dev_runtime_dev_ptr) \
    esl_swimlane_host_onboard_sync_core_types((dev_runtime_dev_ptr))
#define ESL_SWIMLANE_HOST_ONBOARD_END() esl_swimlane_host_onboard_end()

#define ESL_SWIMLANE_PROFILING_FLAG_ON(flags) SET_PROFILING_FLAG((flags), PROFILING_FLAG_L2_SWIMLANE)
#define ESL_SWIMLANE_HOST_SET_LEVEL(level) esl_swimlane_host_set_level(level)
#define ESL_SWIMLANE_HOST_INIT(workers, aicpu_threads, device_id, prefix) \
    esl_swimlane_host_init((workers), (aicpu_threads), (device_id), (prefix))
#define ESL_SWIMLANE_HOST_START() esl_swimlane_host_start()
#define ESL_SWIMLANE_HOST_STOP_EXPORT() esl_swimlane_host_stop_and_export()
#define ESL_SWIMLANE_HOST_FINALIZE() esl_swimlane_host_finalize()
#define ESL_SWIMLANE_HOST_DATA_BASE() esl_swimlane_host_data_base()
#define ESL_SWIMLANE_HOST_ROTATION_TABLE() esl_swimlane_host_rotation_table()
#define ESL_SWIMLANE_HOST_SET_CORE_TYPES(types, count) esl_swimlane_host_set_core_types((types), (count))

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

static inline int esl_swimlane_host_onboard_begin(int device_id, const char *output_prefix)
{
    (void)device_id;
    (void)output_prefix;
    return 0;
}
static inline void esl_swimlane_host_onboard_fill_kargs(void *k_args)
{
    (void)k_args;
}
static inline void esl_swimlane_host_onboard_sync_core_types(void *dev_runtime_dev_ptr)
{
    (void)dev_runtime_dev_ptr;
}
static inline void esl_swimlane_host_onboard_end(void) {}

#define ESL_SWIMLANE_HOST_ONBOARD_BEGIN(device_id, prefix) \
    esl_swimlane_host_onboard_begin((device_id), (prefix))
#define ESL_SWIMLANE_HOST_ONBOARD_FILL_KARGS(k_args_ptr) \
    esl_swimlane_host_onboard_fill_kargs((k_args_ptr))
#define ESL_SWIMLANE_HOST_ONBOARD_SYNC_CORE_TYPES(dev_runtime_dev_ptr) \
    esl_swimlane_host_onboard_sync_core_types((dev_runtime_dev_ptr))
#define ESL_SWIMLANE_HOST_ONBOARD_END() esl_swimlane_host_onboard_end()

#define ESL_SWIMLANE_PROFILING_FLAG_ON(flags) ((void)(flags))
#define ESL_SWIMLANE_HOST_SET_LEVEL(level) ((void)(level))
#define ESL_SWIMLANE_HOST_INIT(workers, aicpu_threads, device_id, prefix) \
    ((void)(workers), (void)(aicpu_threads), (void)(device_id), (void)(prefix), 0)
#define ESL_SWIMLANE_HOST_START() ((void)0)
#define ESL_SWIMLANE_HOST_STOP_EXPORT() ((void)0)
#define ESL_SWIMLANE_HOST_FINALIZE() ((void)0)
#define ESL_SWIMLANE_HOST_DATA_BASE() (0ULL)
#define ESL_SWIMLANE_HOST_ROTATION_TABLE() (0ULL)
#define ESL_SWIMLANE_HOST_SET_CORE_TYPES(types, count) ((void)(types), (void)(count))

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_SWIMLANE_HOST_H */
