#!/usr/bin/env bash
# Build and run esl_proxy onboard smoke — single entry point.
#
# Inlines the three former build scripts (build_aicpu.sh / build_aicore.sh /
# build_onboard_host.sh) as functions and runs the onboard host runner.
# No simpler Python/runtime needed at run time.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
export ESL_PROXY_ROOT="$ROOT"
if [[ -d "${ROOT}/../simpler/simpler_setup" ]]; then
  export SIMPLER_ROOT="$(cd "${ROOT}/../simpler" && pwd)"
fi

ESL_CORE="${ROOT}/esl_proxy"
ONBOARD_INC="${ESL_CORE}/include/platform/onboard"
ONBOARD_SRC="${ESL_CORE}/src/platform/onboard"
ALGO_SRC="${ESL_CORE}/src/algorithm"
SWIMLANE_SRC="${ESL_CORE}/src/swimlane"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
  echo "ASCEND_HOME_PATH must be set" >&2
  exit 1
fi
if [[ ! -d "$ONBOARD_INC" ]]; then
  echo "onboard platform headers missing: $ONBOARD_INC" >&2
  exit 1
fi

SKIP_BUILD=0
DEVICE_ID=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1; shift ;;
    -d|--device) DEVICE_ID="$2"; shift 2 ;;
    -h|--help)
      echo "usage: $0 [-d device_id] [--skip-build]"
      echo "env:  ESL_PROXY_ORCH_CASE=<case>.h  QWEN3_SPMD_TIER=0..4"
      echo "      ESL_PROXY_L2_SWIMLANE_LEVEL=0/1  ASCEND_HOME_PATH=..."
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

if [[ -n "${TASK_DEVICE:-}" ]]; then
  DEVICE_ID="$TASK_DEVICE"
fi

# ---------- shared source lists (build/sources.sh) ----------

# shellcheck disable=SC1091
source "${ROOT}/build/sources.sh"

# ---------- build functions (merged from build/build_*.sh) ----------

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
  local ESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE:-0}"

  local CUSTOM_INCLUDES="${ONBOARD_INC};${ESL_CORE}/include/algorithm;${ESL_CORE}/include/platform;${ESL_CORE}/include/swimlane;${ESL_CORE}/cases"
  local SWIMLANE_FLAGS=""
  if [[ "$ESL_PROXY_ENABLE_L2_SWIMLANE" != "0" ]]; then
    SWIMLANE_FLAGS="-DESL_PROXY_ENABLE_L2_SWIMLANE=1"
  fi

  local SRC_FILES=""
  local src

  for src in "${ESL_PLATFORM_ONBOARD_SRCS[@]}"; do
    SRC_FILES="${SRC_FILES};${ESL_CORE}/${src}"
  done
  for src in "${ESL_ALGORITHM_SRCS[@]}"; do
    SRC_FILES="${SRC_FILES};${ESL_CORE}/${src}"
  done
  SRC_FILES="${SRC_FILES#;}"
  if [[ "$ESL_PROXY_ENABLE_L2_SWIMLANE" != "0" ]]; then
    SRC_FILES="${SRC_FILES};${SWIMLANE_SRC}/swimlane_aicpu.c"
  fi

  export SIMPLER_DISABLE_WARNINGS_AS_ERRORS=1
  local ONBOARD_LOG_FLAGS="-DWORKER_LOG=0 -DMAIN_LOG=0"

  cmake -B "$BUILD_DIR" -S "${ROOT}/build/cmake/aicpu" \
    -DCMAKE_CXX_COMPILER="$CROSS_CXX" \
    -DCMAKE_C_COMPILER="$CROSS_CC" \
    -DCMAKE_C_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} ${SWIMLANE_FLAGS} -Wno-error -w" \
    -DCMAKE_CXX_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} ${SWIMLANE_FLAGS} -Wno-error -w" \
    -DESL_ONBOARD_DIR="$ONBOARD_SRC" \
    -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
    -DCUSTOM_SOURCE_FILES="${SRC_FILES}" \
    -DESCEND_HOME_PATH="$ASCEND_HOME_PATH"

  cmake --build "$BUILD_DIR" -j"$(nproc)"
  echo "Built: ${BUILD_DIR}/libaicpu_kernel.so"
}

build_aicore() {
  local BUILD_DIR="${ROOT}/build/onboard/aicore"
  local AICORE_ENTRY="${ESL_CORE}/src/platform/onboard/aicore_entry.cpp"
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

  local CUSTOM_INCLUDES="${ONBOARD_INC};${ESL_CORE}/include/platform;${ESL_CORE}/include/algorithm;${ESL_CORE}/include/swimlane"
  local ESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE:-0}"

  cmake -B "$BUILD_DIR" -S "${ROOT}/build/cmake/aicore" \
    -DBISHENG_CC="$BISHENG_CC" \
    -DBISHENG_LD="$BISHENG_LD" \
    -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
    -DAICORE_ENTRY_SRC="${AICORE_ENTRY}" \
    -DAICORE_EXECUTOR_SRC="${AICORE_EXECUTOR}" \
    -DESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE}"

  cmake --build "$BUILD_DIR" -j"$(nproc)"
  echo "Built: ${BUILD_DIR}/aicore_kernel.o"
}

build_onboard_host() {
  local BUILD_DIR="${ROOT}/build/onboard/host"
  local ESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE:-0}"

  if [[ -f "${ASCEND_HOME_PATH}/bin/setenv.bash" ]]; then
    # shellcheck disable=SC1090
    source "${ASCEND_HOME_PATH}/bin/setenv.bash"
  fi

  cmake -B "$BUILD_DIR" -S "${ROOT}/build/cmake/host" \
    -DCMAKE_BUILD_TYPE=Release \
    -DASCEND_HOME_PATH="$ASCEND_HOME_PATH" \
    -DONBOARD_INC="$ONBOARD_INC" \
    -DONBOARD_SRC="$ONBOARD_SRC" \
    -DESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE}"

  cmake --build "$BUILD_DIR" -j"$(nproc)"
  echo "Built: ${BUILD_DIR}/esl_onboard_runner"
}

# ---------- build phase ----------

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  SWIMLANE_LEVEL="${ESL_PROXY_L2_SWIMLANE_LEVEL:-0}"
  if [[ "$SWIMLANE_LEVEL" != "0" ]]; then
    export ESL_PROXY_ENABLE_L2_SWIMLANE=1
  fi
  if [[ -n "${ESL_PROXY_ORCH_CASE:-}" ]]; then
    export ORCH_CASE="$ESL_PROXY_ORCH_CASE"
    echo "[esl_proxy] ORCH_CASE=$ORCH_CASE"
  fi
  build_aicpu
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

"$RUNNER" -d "$DEVICE_ID" \
  --dispatcher "$DISPATCHER" \
  --aicpu "$AICPU" \
  --aicore "$AICORE"

# Raw l2_swimlane_records.json is not Perfetto format; runner also emits trace JSON
# when swimlane export succeeds. Re-run conversion here if the runner skipped it.
SWIMLANE_LEVEL="${ESL_PROXY_L2_SWIMLANE_LEVEL:-0}"
RAW_SWIMLANE="${ROOT}/l2_swimlane_records.json"
TRACE_SWIMLANE="${ROOT}/l2_swimlane_trace.json"
if [[ "$SWIMLANE_LEVEL" != "0" && -f "$RAW_SWIMLANE" && ! -f "$TRACE_SWIMLANE" ]]; then
  python3 "${ROOT}/tools/swimlane_to_perfetto.py" "$RAW_SWIMLANE" -o "$TRACE_SWIMLANE" || true
fi
