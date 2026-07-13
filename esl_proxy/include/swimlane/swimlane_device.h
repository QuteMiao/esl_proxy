/*
 * Device-side swimlane API: AICPU collection + AICore profiling state/record.
 */
#ifndef ESL_PROXY_SWIMLANE_DEVICE_H
#define ESL_PROXY_SWIMLANE_DEVICE_H

#include "swimlane_types.h"
#include "tools.h"

#include <stdbool.h>
#include <stdint.h>

static inline uint64_t get_sys_cnt_aicpu(void)
{
    return esl_onboard_sys_cnt();
}

#ifdef __cplusplus
extern "C" {
#endif

void set_platform_l2_swimlane_base(uint64_t l2_swimlane_data_base);
uint64_t get_platform_l2_swimlane_base(void);
void set_l2_swimlane_enabled(bool enable);
bool is_l2_swimlane_enabled(void);
void set_platform_l2_swimlane_aicore_rotation_table(uint64_t table_addr);
uint64_t get_platform_l2_swimlane_aicore_rotation_table(void);
void l2_swimlane_aicpu_init(int worker_count);
void l2_swimlane_aicpu_on_aicore_dispatch(int core_id, int thread_idx);
void l2_swimlane_aicpu_flush(int thread_idx, const int *cur_thread_cores, int core_num);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_SWIMLANE_DEVICE_H */
