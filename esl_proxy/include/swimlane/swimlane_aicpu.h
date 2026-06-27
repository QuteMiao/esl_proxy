/*
 * L2 swimlane AICPU hooks (aicpu_runtime.c and related .c sources).
 */
#ifndef ESL_PROXY_SWIMLANE_AICPU_H
#define ESL_PROXY_SWIMLANE_AICPU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct EslRuntime EslRuntime;

#include "platform_config.h"
#include "runtime.h"

#if ESL_PROXY_ENABLE_L2_SWIMLANE

#ifdef __cplusplus
extern "C" {
#endif

void set_platform_l2_swimlane_base(uint64_t l2_swimlane_data_base);
void set_platform_l2_swimlane_aicore_rotation_table(uint64_t table_addr);
void set_l2_swimlane_enabled(bool enable);
bool is_l2_swimlane_enabled(void);
void l2_swimlane_aicpu_init(int worker_count);
void l2_swimlane_aicpu_on_aicore_dispatch(int core_id, int thread_idx);
void l2_swimlane_aicpu_flush(int thread_idx, const int *cur_thread_cores, int core_num);

#ifdef __cplusplus
}
#endif

#define ESL_SWIMLANE_IS_FLAG_ON(flags) GET_PROFILING_FLAG((flags), PROFILING_FLAG_L2_SWIMLANE)
#define ESL_SWIMLANE_AICPU_SET_BASE(addr) set_platform_l2_swimlane_base(addr)
#define ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(addr) set_platform_l2_swimlane_aicore_rotation_table(addr)
#define ESL_SWIMLANE_AICPU_SET_ENABLED(on) set_l2_swimlane_enabled(on)
#define ESL_SWIMLANE_AICPU_INIT(workers) l2_swimlane_aicpu_init(workers)
#define ESL_SWIMLANE_AICPU_FLUSH(thread_idx, cores, core_num) \
    l2_swimlane_aicpu_flush((thread_idx), (cores), (core_num))
#define ESL_SWIMLANE_AICPU_ON_DISPATCH(core_id, thread_idx) \
    l2_swimlane_aicpu_on_aicore_dispatch((core_id), (thread_idx))

#define ESL_SWIMLANE_AICPU_SHUTDOWN_FLUSH(runtime) \
    do { \
        EslRuntime *_esl_sw_rt = (runtime); \
        if (_esl_sw_rt != NULL) { \
            int _esl_sw_n = _esl_sw_rt->worker_count; \
            int _esl_sw_cores[RUNTIME_MAX_WORKER]; \
            int _esl_sw_i; \
            if (_esl_sw_n > RUNTIME_MAX_WORKER) { \
                _esl_sw_n = RUNTIME_MAX_WORKER; \
            } \
            for (_esl_sw_i = 0; _esl_sw_i < _esl_sw_n; ++_esl_sw_i) { \
                _esl_sw_cores[_esl_sw_i] = _esl_sw_i; \
            } \
            ESL_SWIMLANE_AICPU_FLUSH(ESL_AICPU_ROLE_DISPATCH, _esl_sw_cores, _esl_sw_n); \
        } \
    } while (0)

#else /* !ESL_PROXY_ENABLE_L2_SWIMLANE */

#define ESL_SWIMLANE_IS_FLAG_ON(flags) (0)
#define ESL_SWIMLANE_AICPU_SET_BASE(addr) ((void)(addr))
#define ESL_SWIMLANE_AICPU_SET_ROTATION_TABLE(addr) ((void)(addr))
#define ESL_SWIMLANE_AICPU_SET_ENABLED(on) ((void)(on))
#define ESL_SWIMLANE_AICPU_INIT(workers) ((void)(workers))
#define ESL_SWIMLANE_AICPU_FLUSH(thread_idx, cores, core_num) \
    ((void)(thread_idx), (void)(cores), (void)(core_num))
#define ESL_SWIMLANE_AICPU_ON_DISPATCH(core_id, thread_idx) ((void)(core_id), (void)(thread_idx))
#define ESL_SWIMLANE_AICPU_SHUTDOWN_FLUSH(runtime) ((void)(runtime))

#endif /* ESL_PROXY_ENABLE_L2_SWIMLANE */

#endif /* ESL_PROXY_SWIMLANE_AICPU_H */
