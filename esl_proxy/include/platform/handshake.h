/*
 * handshake.h — AICPU↔AICore handshake (platform layer).
 *
 * Implemented in src/platform/handshake.c. Depends on runtime.h and a2a3
 * platform_config; platform tools headers must not include this header
 * (one-way layering).
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
/* Precomputed COND-register pointer for core_idx (NULL until handshake resolves it,
 * or for out-of-range indices). Lets the dispatch poll skip reg_offset/base math. */
volatile uint32_t *esl_handshake_cond_ptr(int core_idx);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_ALGORITHM_HANDSHAKE_H */
