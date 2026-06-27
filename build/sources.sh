# Shared esl_proxy source lists — sourced by Makefile and tools/run_onboard.sh.
#
# Paths are relative to esl_proxy/esl_proxy/ (ESL_CORE).

ESL_ALGORITHM_SRCS=(
  src/algorithm/dispatch.c
  src/algorithm/dispatch_expand.c
  src/algorithm/cutter.c
  src/algorithm/executor.c
  src/algorithm/handshake.c
  src/algorithm/shm.c
)

ESL_ALGORITHM_SIM_EXTRA_SRCS=(
  src/algorithm/manager.c
  src/algorithm/log.c
  src/algorithm/aicore_executor.c
)

ESL_SIM_CXX_SRCS=(
  src/platform/sim/aicore_wrapper.cpp
)

ESL_PLATFORM_SIM_SRCS=(
  src/platform/sim/platform_init.c
  src/platform/sim/platform_regs.c
  src/platform/sim/cache_ops.c
  src/platform/sim/platform_sim.c
  src/platform/sim/sim_core_regs.c
  src/platform/sim/aicore.c
  src/platform/sim/device_runner.c
)

ESL_PLATFORM_ONBOARD_SRCS=(
  src/platform/onboard/npu_hal.c
  src/platform/onboard/cache_ops.c
  src/platform/onboard/onboard_log.c
  src/platform/onboard/tools.c
  src/platform/onboard/platform_init.c
  src/platform/onboard/aicpu_runtime.c
  src/platform/onboard/platform_sync_onboard.c
)
