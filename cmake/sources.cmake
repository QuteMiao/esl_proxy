# Shared esl_proxy AICPU-kernel source lists.
# Paths are relative to esl_proxy/ (ESL_CORE); the includer prefixes ESL_CORE.
# Included by cmake/aicpu/CMakeLists.txt. The sim build (esl_proxy/Makefile) keeps
# its own list.

# dispatch has two interchangeable variants — select one (default: basic).
#   ESL_PROXY_DOUBLE_BUFFER=OFF -> dispatch.c             (basic single-buffer)
#   ESL_PROXY_DOUBLE_BUFFER=ON  -> dispatch_double_buffer.c (2-outstanding/core)
if(NOT DEFINED ESL_PROXY_DOUBLE_BUFFER)
    set(ESL_PROXY_DOUBLE_BUFFER OFF)
endif()
if(ESL_PROXY_DOUBLE_BUFFER)
    set(ESL_DISPATCH_SRC src/algorithm/dispatch_double_buffer.c)
else()
    set(ESL_DISPATCH_SRC src/algorithm/dispatch.c)
endif()

set(ESL_ALGORITHM_SRCS
    ${ESL_DISPATCH_SRC}
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
