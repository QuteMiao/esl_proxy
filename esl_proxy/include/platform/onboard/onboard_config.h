/*
 * onboard_config.h — onboard-only configuration.
 *
 * The platform config shared with the sim backend lives in the neutral
 * include/platform/platform_config.h; this header pulls it in and adds the
 * onboard-only CANN host knobs.
 */
#ifndef ESL_PROXY_ONBOARD_CONFIG_H
#define ESL_PROXY_ONBOARD_CONFIG_H

#include "platform_config.h"

/* --- onboard-only (CANN host) --- */
#define PLATFORM_OP_EXECUTE_TIMEOUT_US 3000000ULL
#define PLATFORM_STREAM_SYNC_TIMEOUT_MS 15000

#endif /* ESL_PROXY_ONBOARD_CONFIG_H */
