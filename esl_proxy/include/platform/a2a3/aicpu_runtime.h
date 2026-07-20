/*
 * Onboard AICPU functional layer API (orchestration + platform init hooks).
 */
#ifndef ESL_PROXY_AICPU_RUNTIME_H
#define ESL_PROXY_AICPU_RUNTIME_H

#include <stdint.h>

typedef struct EslRuntime EslRuntime;

#ifdef __cplusplus
extern "C" {
#endif

int esl_platform_init(EslRuntime *runtime);
void esl_platform_shutdown(EslRuntime *runtime);
int32_t esl_aicpu_execute(EslRuntime *runtime);
void esl_write_stats(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt, uint64_t commit,
                     uint64_t ready_cube, uint64_t ready_vec, uint64_t min_uncomplete);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_AICPU_RUNTIME_H */
