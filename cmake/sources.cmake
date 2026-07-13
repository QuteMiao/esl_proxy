# Shared esl_proxy AICPU-kernel source lists.
# Paths are relative to esl_proxy/ (ESL_CORE); the includer prefixes ESL_CORE.
# Included by cmake/aicpu/CMakeLists.txt. The sim build (esl_proxy/Makefile) keeps
# its own list.

# dispatch.c holds basic + SPMD/MIX + double-buffer (selected via
# -DESL_DISPATCH_DOUBLE_BUFFER=1). ESL_PROXY_DOUBLE_BUFFER=ON sets that define
# on aicpu_kernel (see cmake/aicpu/CMakeLists.txt).
set(ESL_ALGORITHM_SRCS
    src/algorithm/dispatch.c
    src/algorithm/dispatch_payload.c
    src/algorithm/cutter.c
    src/algorithm/executor.c
    src/algorithm/handshake.c
    src/algorithm/shm.c
)

set(ESL_PLATFORM_ONBOARD_SRCS
    src/platform/onboard/npu_hal.c
    src/platform/onboard/cache_ops.c
    src/platform/onboard/onboard_log.c
    src/platform/onboard/tools.c
    src/platform/onboard/platform_init.c
    src/platform/onboard/aicpu_runtime.c
    src/platform/onboard/aicpu_affinity.c
    src/platform/onboard/platform_sync_onboard.c
)
