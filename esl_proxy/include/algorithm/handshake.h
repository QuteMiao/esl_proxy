/*
 * handshake.h — AICPU↔AICore handshake + dispatch-payload prep (algorithm layer).
 *
 * Backend-neutral: implemented in src/algorithm/handshake.c and
 * src/algorithm/dispatch.c, compiled by both the onboard and sim backends.
 * Depends downward on the platform HAL (runtime.h, onboard_config.h, platform_regs.h);
 * platform headers must NOT include this header (one-way layering).
 */
#ifndef ESL_PROXY_ALGORITHM_HANDSHAKE_H
#define ESL_PROXY_ALGORITHM_HANDSHAKE_H

#include <stdint.h>

#include "runtime.h"
#include "platform_config.h"

#ifdef __cplusplus
extern "C" {
#endif

int esl_handshake_start(EslRuntime *runtime);
int esl_handshake_all_cores(EslRuntime *runtime);
void esl_shutdown_all_cores(EslRuntime *runtime);
uint64_t esl_handshake_reg_addr(int core_idx);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ALGORITHM_HANDSHAKE_H */
