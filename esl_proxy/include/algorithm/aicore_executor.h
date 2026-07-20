/*
 * aicore_executor.h — AICore handshake + dispatch loop (algorithm layer).
 */
#ifndef ESL_PROXY_ALGORITHM_AICORE_EXECUTOR_H
#define ESL_PROXY_ALGORITHM_AICORE_EXECUTOR_H

#include "aicore.h"
#include "core_type.h"
#include "runtime.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

__aicore__ __attribute__((weak)) void aicore_execute(__gm__ EslRuntime *runtime, int block_idx,
                                                      CoreType worker_core_type);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ALGORITHM_AICORE_EXECUTOR_H */
