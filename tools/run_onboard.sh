#!/usr/bin/env bash
# Build and run esl_proxy onboard — single entry point (no swimlane).
#
# Modes:
#   bash tools/run_onboard.sh                       # build + run default case
#   bash tools/run_onboard.sh --all-cases           # iterate ALL_CASES
#
# Options:
#   --basic               basic single-buffer dispatch (default)
#   --double-buffer,--db  double-buffer dispatch (2 outstanding/core)
#   -d, --device <id>     device id (default: 0 or $TASK_DEVICE)
#   --skip-build          reuse existing build artifacts
#   --all-cases, -a       iterate ALL_CASES
#   --case <file.h>       override single case (default: paged_attention_unroll_manual_scope.h)
#   --cooldown <sec>      seconds between cases in --all-cases (default: 30)
#   -h, --help            show this help
#
# Env:
#   ASCEND_HOME_PATH=<...>            (required for build/aicore)
#   ESL_PROXY_ORCH_CASE=<case>.h      (overrides --case)
#   QWEN3_SPMD_TIER=0..4              (default 0)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
export ESL_PROXY_ROOT="$ROOT"
if [[ -d "${ROOT}/../simpler/simpler_setup" ]]; then
  export SIMPLER_ROOT="$(cd "${ROOT}/../simpler" && pwd)"
fi

ESL_CORE="${ROOT}/esl_proxy"
ONBOARD_INC="${ESL_CORE}/include/platform/a2a3"
ONBOARD_SRC="${ESL_CORE}/src/platform/a2a3"
PLATFORM_INC="${ESL_CORE}/include/platform"
PLATFORM_TOOLS_INC="${ESL_CORE}/include/platform/tools"

ALL_CASES=(
    paged_attention_unroll.h
    paged_attention_unroll_manual_scope.h
)
DEFAULT_CASE="paged_attention_unroll_manual_scope.h"

# ---------- arg parsing ----------
SKIP_BUILD=0
DEVICE_ID=0
ALL_CASES_FLAG=0
CASE_OVERRIDE=""
COOLDOWN=30
DOUBLE_BUFFER=OFF

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1; shift ;;
    --basic) DOUBLE_BUFFER=OFF; shift ;;
    --double-buffer|--db) DOUBLE_BUFFER=ON; shift ;;
    -d|--device) DEVICE_ID="$2"; shift 2 ;;
    --all-cases|-a) ALL_CASES_FLAG=1; shift ;;
    --case) CASE_OVERRIDE="$2"; shift 2 ;;
    --cooldown) COOLDOWN="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,22p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

if [[ -n "${TASK_DEVICE:-}" ]]; then
  DEVICE_ID="$TASK_DEVICE"
fi

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH must be set" >&2
  exit 1
fi
if [[ ! -d "$ONBOARD_INC" ]]; then
  echo "onboard platform headers missing: $ONBOARD_INC" >&2
  exit 1
fi

# ---------- build functions ----------

build_aicpu() {
  local BUILD_DIR="${ROOT}/build/onboard/aicpu"

  local CROSS_CXX="${ASCEND_HOME_PATH}/tools/hcc/bin/aarch64-target-linux-gnu-g++"
  if [[ ! -x "$CROSS_CXX" ]]; then
    CROSS_CXX="$(command -v aarch64-target-linux-gnu-g++ || true)"
  fi
  local CROSS_CC="${CROSS_CXX/g++/gcc}"
  if [[ ! -x "$CROSS_CC" ]]; then
    CROSS_CC="$(command -v aarch64-target-linux-gnu-gcc || true)"
  fi
  if [[ -z "$CROSS_CXX" || -z "$CROSS_CC" ]]; then
    echo "aarch64 cross compiler not found" >&2
    exit 1
  fi

  if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    local cached
    cached="$(grep -m1 '^CMAKE_CXX_COMPILER:FILEPATH=' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true)"
    if [[ -n "$cached" && "$cached" != "$CROSS_CXX" ]]; then
      rm -rf "$BUILD_DIR"
    fi
  fi

  local ORCH_CASE="${ORCH_CASE:-paged_attention_unroll_manual_scope.h}"
  local QWEN3_SPMD_TIER="${QWEN3_SPMD_TIER:-0}"
  local CUSTOM_INCLUDES="${ONBOARD_INC};${PLATFORM_TOOLS_INC};${PLATFORM_INC};${ESL_CORE}/include/algorithm;${ESL_CORE}/cases"

  export SIMPLER_DISABLE_WARNINGS_AS_ERRORS=1
  local ONBOARD_LOG_FLAGS="-DWORKER_LOG=0 -DMAIN_LOG=0"
  # a2a3: override conf.h default AIC_CNT=60 → 24
  local AIC_CNT_FLAG="-DAIC_CNT=24"

  cmake -B "$BUILD_DIR" -S "${ROOT}/cmake/aicpu" \
    -DCMAKE_CXX_COMPILER="$CROSS_CXX" \
    -DCMAKE_C_COMPILER="$CROSS_CC" \
    -DCMAKE_C_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} ${AIC_CNT_FLAG} -Wno-error -w" \
    -DCMAKE_CXX_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} ${AIC_CNT_FLAG} -Wno-error -w" \
    -DESL_ONBOARD_DIR="$ONBOARD_SRC" \
    -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
    -DESL_PROXY_DOUBLE_BUFFER="${DOUBLE_BUFFER}" \
    -DESCEND_HOME_PATH="$ASCEND_HOME_PATH"

  cmake --build "$BUILD_DIR" -j"$(nproc)"
  echo "Built: ${BUILD_DIR}/libaicpu_kernel.so"
}

build_aicore() {
  local BUILD_DIR="${ROOT}/build/onboard/aicore"
  local AICORE_ENTRY="${ESL_CORE}/src/platform/a2a3/aicore_entry.cpp"
  local AICORE_EXECUTOR="${ESL_CORE}/src/algorithm/aicore_executor.c"

  local BISHENG_CC
  BISHENG_CC="$(command -v ccec || echo "${ASCEND_HOME_PATH}/compiler/ccec_compiler/bin/ccec")"
  local BISHENG_LD
  BISHENG_LD="$(command -v ld.lld || echo "${ASCEND_HOME_PATH}/compiler/ccec_compiler/bin/ld.lld")"
  if [[ ! -x "$BISHENG_CC" || ! -x "$BISHENG_LD" ]]; then
    echo "ccec/ld.lld not found under ASCEND_HOME_PATH" >&2
    exit 1
  fi

  if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    local cached
    cached="$(grep -m1 '^BISHENG_CC:FILEPATH=' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true)"
    if [[ -n "$cached" && "$cached" != "$BISHENG_CC" ]]; then
      rm -rf "$BUILD_DIR"
    fi
  fi

  local CUSTOM_INCLUDES="${ONBOARD_INC};${PLATFORM_TOOLS_INC};${PLATFORM_INC};${ESL_CORE}/include/algorithm"

  cmake -B "$BUILD_DIR" -S "${ROOT}/cmake/aicore" \
    -DBISHENG_CC="$BISHENG_CC" \
    -DBISHENG_LD="$BISHENG_LD" \
    -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
    -DAICORE_ENTRY_SRC="${AICORE_ENTRY}" \
    -DAICORE_EXECUTOR_SRC="${AICORE_EXECUTOR}" \

  cmake --build "$BUILD_DIR" -j"$(nproc)"
  echo "Built: ${BUILD_DIR}/aicore_kernel.o"
}

build_onboard_host() {
  local BUILD_DIR="${ROOT}/build/onboard/host"

  if [[ -f "${ASCEND_HOME_PATH}/bin/setenv.bash" ]]; then
    # shellcheck disable=SC1090
    source "${ASCEND_HOME_PATH}/bin/setenv.bash"
  elif [[ -f "${ASCEND_HOME_PATH}/set_env.sh" ]]; then
    # shellcheck disable=SC1090
    source "${ASCEND_HOME_PATH}/set_env.sh"
  fi

  cmake -B "$BUILD_DIR" -S "${ROOT}/cmake/host" \
    -DCMAKE_BUILD_TYPE=Release \
    -DASCEND_HOME_PATH="$ASCEND_HOME_PATH" \
    -DONBOARD_INC="$ONBOARD_INC" \
    -DONBOARD_SRC="$ONBOARD_SRC" \

  cmake --build "$BUILD_DIR" -j"$(nproc)"
  echo "Built: ${BUILD_DIR}/esl_onboard_runner"
}

run_one_case() {
    local case="$1"

    export ORCH_CASE="$case"
    export ESL_PROXY_ORCH_CASE="$case"
    echo "[esl_proxy] ORCH_CASE=$case device=${DEVICE_ID}"

    "${ROOT}/build/onboard/host/esl_onboard_runner" \
        -d "$DEVICE_ID" \
        --dispatcher "${ROOT}/build/onboard/aicpu/libesl_aicpu_dispatcher.so" \
        --aicpu "${ROOT}/build/onboard/aicpu/libaicpu_kernel.so" \
        --aicore "${ROOT}/build/onboard/aicore/aicore_kernel.o"
}

# ---------- build phase ----------

if [[ "$SKIP_BUILD" -eq 0 && "$ALL_CASES_FLAG" -eq 0 ]]; then
  if [[ -n "${ESL_PROXY_ORCH_CASE:-}" ]]; then
    export ORCH_CASE="$ESL_PROXY_ORCH_CASE"
  fi
  build_aicpu
  build_aicore
  build_onboard_host
fi

if [[ "$ALL_CASES_FLAG" -ne 0 && "$SKIP_BUILD" -eq 0 ]]; then
  build_aicore
  build_onboard_host
fi

# ---------- run phase ----------

RUNNER="${ROOT}/build/onboard/host/esl_onboard_runner"
DISPATCHER="${ROOT}/build/onboard/aicpu/libesl_aicpu_dispatcher.so"
AICPU="${ROOT}/build/onboard/aicpu/libaicpu_kernel.so"
AICORE="${ROOT}/build/onboard/aicore/aicore_kernel.o"

for f in "$RUNNER" "$DISPATCHER" "$AICPU" "$AICORE"; do
  if [[ ! -f "$f" ]]; then
    echo "missing $f (run without --skip-build first)" >&2
    exit 1
  fi
done

if [[ "$ALL_CASES_FLAG" -ne 0 ]]; then
    for case in "${ALL_CASES[@]}"; do
        echo "======== CASE ${case} ========"
        export ORCH_CASE="$case"
        export ESL_PROXY_ORCH_CASE="$case"
        build_aicpu
        run_one_case "$case"
        if [[ "$COOLDOWN" -gt 0 ]]; then
            echo "Waiting ${COOLDOWN}s for device cooldown..."
            sleep "$COOLDOWN"
        fi
    done
    echo "ALL DONE"
else
    single_case="${CASE_OVERRIDE:-${ESL_PROXY_ORCH_CASE:-$DEFAULT_CASE}}"
    run_one_case "$single_case"
fi
