#!/usr/bin/env bash
# One-shot: build+run all onboard cases under task-submit ($TASK_DEVICE).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export ASCEND_HOME_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.0.0}"
export QWEN3_SPMD_TIER="${QWEN3_SPMD_TIER:-2}"

if [[ -f "${ASCEND_HOME_PATH}/bin/setenv.bash" ]]; then
  # shellcheck disable=SC1090
  source "${ASCEND_HOME_PATH}/bin/setenv.bash"
elif [[ -f "${ASCEND_HOME_PATH}/set_env.sh" ]]; then
  # shellcheck disable=SC1090
  source "${ASCEND_HOME_PATH}/set_env.sh"
fi

DEVICE_ID="${TASK_DEVICE:-${1:-0}}"
echo "TASK_DEVICE=${DEVICE_ID} ASCEND_HOME_PATH=${ASCEND_HOME_PATH} QWEN3_SPMD_TIER=${QWEN3_SPMD_TIER}"

CASES=(
  paged_attention_unroll.h
  paged_attention_unroll_manual_scope.h
  qwen3_dynamic_manual_scope.h
  qwen3_dynamic_tensormap.h
)

FAIL=0
PASSED=()
FAILED=()

for case in "${CASES[@]}"; do
  echo ""
  echo "############################################"
  echo "# CASE: ${case}"
  echo "############################################"
  export ESL_PROXY_ORCH_CASE="$case"
  export ORCH_CASE="$case"
  if bash tools/run_onboard.sh --case "$case" -d "$DEVICE_ID"; then
    echo "[PASS] ${case}"
    PASSED+=("$case")
  else
    rc=$?
    echo "[FAIL] ${case} exit=${rc}"
    FAILED+=("$case")
    FAIL=1
  fi
done

echo ""
echo "======== SUMMARY ========"
echo "PASSED (${#PASSED[@]}): ${PASSED[*]:-}"
echo "FAILED (${#FAILED[@]}): ${FAILED[*]:-}"
if [[ "$FAIL" -ne 0 ]]; then
  exit 1
fi
echo "ALL CASES PASSED"
