#!/usr/bin/env bash
# GDB debug for esl_onboard_runner (no simpler).
set -euo pipefail
source "$ASCEND_HOME_PATH/bin/setenv.bash"
ROOT=/data/y00955915/Desktop/esl_proxy_main/esl_proxy
cd "$ROOT"

RUNNER="${ROOT}/build/onboard/host/esl_onboard_runner"
DISPATCHER="${ROOT}/build/onboard/aicpu/libesl_aicpu_dispatcher.so"
AICPU="${ROOT}/build/onboard/aicpu/libaicpu_kernel.so"
DEVICE="${TASK_DEVICE:-0}"

ulimit -c unlimited
GDB="${GDB:-gdb}"

exec "$GDB" -batch \
  -ex "set pagination off" \
  -ex "run" \
  -ex "bt full" \
  -ex "thread apply all bt" \
  -ex "quit" \
  --args "$RUNNER" -d "$DEVICE" --dispatcher "$DISPATCHER" --aicpu "$AICPU"
