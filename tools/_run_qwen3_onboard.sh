#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
export ASCEND_HOME_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.0.0}"
if [[ -f "${ASCEND_HOME_PATH}/bin/setenv.bash" ]]; then
  # shellcheck disable=SC1090
  source "${ASCEND_HOME_PATH}/bin/setenv.bash"
fi
DEVICE_ID="${TASK_DEVICE:-0}"
FAIL=0
for tier in 0 2; do
  for case in qwen3_dynamic_manual_scope.h qwen3_dynamic_tensormap.h; do
    echo "===== ${case} QWEN3_SPMD_TIER=${tier} ====="
    export QWEN3_SPMD_TIER="$tier"
    export ESL_PROXY_ORCH_CASE="$case"
    export ORCH_CASE="$case"
    if bash tools/run_onboard.sh --case "$case" -d "$DEVICE_ID"; then
      echo "[PASS] ${case} tier=${tier}"
    else
      echo "[FAIL] ${case} tier=${tier} rc=$?"
      FAIL=1
    fi
    sleep 5
  done
done
exit "$FAIL"
