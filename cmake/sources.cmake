# Shared esl_proxy AICPU-kernel source lists.
# Paths are relative to esl_proxy/ (ESL_CORE); the includer prefixes ESL_CORE.
# Included by cmake/aicpu/CMakeLists.txt. CPU Fake Return (esl_proxy/Makefile) keeps
# its own list (dispatch.c).

# a2a3 uses dispatch_a3.c (doorbell + COND; payload prepare/publish inlined).
# Legacy Fake Return remains in dispatch.c for CPU Makefile builds (PR #13).
set(ESL_ALGORITHM_SRCS
    src/algorithm/dispatch_a3.c
    src/algorithm/cutter.c
    src/algorithm/executor.c
    src/algorithm/shm.c
)

set(ESL_PLATFORM_A2A3_SRCS
    src/platform/handshake.c
    src/platform/a2a3/npu_hal.c
    src/platform/a2a3/cache_ops.c
    src/platform/a2a3/onboard_log.c
    src/platform/a2a3/tools.c
    src/platform/a2a3/platform_init.c
    src/platform/a2a3/aicpu_runtime.c
    src/platform/a2a3/platform_sync_onboard.c
)

# Back-compat alias for older cmake snippets.
set(ESL_PLATFORM_ONBOARD_SRCS ${ESL_PLATFORM_A2A3_SRCS})
