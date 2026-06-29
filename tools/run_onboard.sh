#!/usr/bin/env bash
# Build and run esl_proxy onboard — single entry point.
#
# Inlines the three former build scripts (build_aicpu.sh / build_aicore.sh /
# build_onboard_host.sh) and the two former swimlane batch runners
# (run_onboard_swimlane_cases.sh / run_swimlane_all_cases.sh).
#
# Modes:
#   bash tools/run_onboard.sh                       # build + run default case
#   bash tools/run_onboard.sh --swimlane            # build + run + L2 swimlane trace + perf summary
#   bash tools/run_onboard.sh --all-cases           # run all 4 ORCH cases
#   bash tools/run_onboard.sh --all-cases --swimlane  # → report/swimlane/basic/<case>/
#   bash tools/run_onboard.sh --all-cases --swimlane --double-buffer  # → report/swimlane/double_buffer/<case>/
#
# Options:
#   --npu                 build + run on a real NPU via task-submit (device auto)
#   --basic               basic single-buffer dispatch.c (default)
#   --double-buffer,--db  double-buffer dispatch (2 outstanding/core)
#   -d, --device <id>     device id (default: 0 or $TASK_DEVICE)
#   --skip-build          reuse existing build artifacts
#   --swimlane, -S        enable L2 swimlane trace export (sets level=1)
#   --all-cases, -a       iterate all 4 ORCH cases
#   --case <file.h>       override single case (default: paged_attention_unroll_manual_scope.h)
#   --cooldown <sec>      seconds between cases in --all-cases (default: 30)
#   --no-summary          skip perf summary when --swimlane is set
#   -h, --help            show this help
#
# Env:
#   ASCEND_HOME_PATH=<...>            (required for build/aicore)
#   ESL_PROXY_ORCH_CASE=<case>.h      (overrides --case)
#   QWEN3_SPMD_TIER=0..4              (default 0)
#   ESL_PROXY_L2_SWIMLANE_LEVEL=0/1   (overrides --swimlane)
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
SWIMLANE_SRC="${ESL_CORE}/src/swimlane"

ALL_CASES=(
    qwen3_dynamic_manual_scope.h
    qwen3_dynamic_tensormap.h
    paged_attention_unroll.h
    paged_attention_unroll_manual_scope.h
)
DEFAULT_CASE="paged_attention_unroll_manual_scope.h"

# ---------- arg parsing ----------
SKIP_BUILD=0
DEVICE_ID=0
SWIMLANE_FLAG=0
ALL_CASES_FLAG=0
CASE_OVERRIDE=""
COOLDOWN=30
NO_SUMMARY=0
DOUBLE_BUFFER=OFF  # dispatch variant; default basic, --double-buffer selects double-buffer

# --npu: build + run the whole thing on a real NPU via task-submit (former
# run_onboard_npu.sh). Re-exec self with --npu stripped; $TASK_DEVICE supplies
# the device. SKIP_BUILD=1 env is bridged to --skip-build across the boundary.
_npu=0
_npu_args=()
for _a in "$@"; do
  if [[ "$_a" == "--npu" ]]; then _npu=1; else _npu_args+=("$_a"); fi
done
if [[ "$_npu" == "1" ]]; then
  if [[ "${SKIP_BUILD:-0}" == "1" && ! " ${_npu_args[*]} " == *" --skip-build "* ]]; then
    _npu_args+=("--skip-build")
  fi
  _ascend_env="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.0.0}/bin/setenv.bash"
  exec task-submit --device auto --max-time 0 --timeout 3600 \
    --env ASCEND_HOME_PATH --env PATH --env LD_LIBRARY_PATH --env HOME --env USER \
    --run "source '${_ascend_env}' && cd '${ROOT}' && bash tools/run_onboard.sh ${_npu_args[*]}"
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1; shift ;;
    --basic) DOUBLE_BUFFER=OFF; shift ;;
    --double-buffer|--db) DOUBLE_BUFFER=ON; shift ;;
    -d|--device) DEVICE_ID="$2"; shift 2 ;;
    --swimlane|-S) SWIMLANE_FLAG=1; shift ;;
    --all-cases|-a) ALL_CASES_FLAG=1; shift ;;
    --case) CASE_OVERRIDE="$2"; shift 2 ;;
    --cooldown) COOLDOWN="$2"; shift 2 ;;
    --no-summary) NO_SUMMARY=1; shift ;;
    -h|--help)
      sed -n '2,30p' "${BASH_SOURCE[0]}"
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

# Honor env overrides for swimlane level.
if [[ -n "${ESL_PROXY_L2_SWIMLANE_LEVEL:-}" ]]; then
  SWIMLANE_FLAG=1
fi
if [[ "$SWIMLANE_FLAG" -ne 0 ]]; then
  export ESL_PROXY_L2_SWIMLANE_LEVEL=1
  export ESL_PROXY_ENABLE_L2_SWIMLANE=1
fi

# ---------- build functions (merged from former build_*.sh) ----------
# AICPU kernel source list lives in cmake/sources.cmake (included by cmake/aicpu).

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

  export SIMPLER_DISABLE_WARNINGS_AS_ERRORS=1
  local ONBOARD_LOG_FLAGS="-DWORKER_LOG=0 -DMAIN_LOG=0"

  cmake -B "$BUILD_DIR" -S "${ROOT}/cmake/aicpu" \
    -DCMAKE_CXX_COMPILER="$CROSS_CXX" \
    -DCMAKE_C_COMPILER="$CROSS_CC" \
    -DCMAKE_C_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} ${SWIMLANE_FLAGS} -Wno-error -w" \
    -DCMAKE_CXX_FLAGS="-DESL_PROXY_ONBOARD -DORCH_CASE=${ORCH_CASE} -DQWEN3_SPMD_TIER=${QWEN3_SPMD_TIER} ${ONBOARD_LOG_FLAGS} ${SWIMLANE_FLAGS} -Wno-error -w" \
    -DESL_ONBOARD_DIR="$ONBOARD_SRC" \
    -DCUSTOM_INCLUDE_DIRS="${CUSTOM_INCLUDES}" \
    -DESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE}" \
    -DESL_PROXY_DOUBLE_BUFFER="${DOUBLE_BUFFER}" \
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

  cmake -B "$BUILD_DIR" -S "${ROOT}/cmake/aicore" \
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

  cmake -B "$BUILD_DIR" -S "${ROOT}/cmake/host" \
    -DCMAKE_BUILD_TYPE=Release \
    -DASCEND_HOME_PATH="$ASCEND_HOME_PATH" \
    -DONBOARD_INC="$ONBOARD_INC" \
    -DONBOARD_SRC="$ONBOARD_SRC" \
    -DESL_PROXY_ENABLE_L2_SWIMLANE="${ESL_PROXY_ENABLE_L2_SWIMLANE}"

  cmake --build "$BUILD_DIR" -j"$(nproc)"
  echo "Built: ${BUILD_DIR}/esl_onboard_runner"
}

# ---------- helpers ----------

resolve_func_names_json() {
    local case="$1"
    case "$case" in
        paged_attention_*) echo "${ROOT}/tools/paged_attention_func_names.json" ;;
        qwen3_*)           echo "${ROOT}/tools/qwen3_func_names.json" ;;
        *)                 echo "" ;;
    esac
}

run_one_case() {
    local case="$1"
    local out_dir="${2:-}"

    export ORCH_CASE="$case"
    export ESL_PROXY_ORCH_CASE="$case"
    echo "[esl_proxy] ORCH_CASE=$case (swimlane=${SWIMLANE_FLAG})"

    rm -f "${ROOT}/l2_swimlane_records.json" "${ROOT}/l2_swimlane_trace.json"

    local log=""
    if [[ -n "$out_dir" ]]; then
        mkdir -p "$out_dir"
        log="${out_dir}/run.log"
        "${ROOT}/build/onboard/host/esl_onboard_runner" \
            -d "$DEVICE_ID" \
            --dispatcher "${ROOT}/build/onboard/aicpu/libesl_aicpu_dispatcher.so" \
            --aicpu "${ROOT}/build/onboard/aicpu/libaicpu_kernel.so" \
            --aicore "${ROOT}/build/onboard/aicore/aicore_kernel.o" 2>&1 | tee "$log"
    else
        "${ROOT}/build/onboard/host/esl_onboard_runner" \
            -d "$DEVICE_ID" \
            --dispatcher "${ROOT}/build/onboard/aicpu/libesl_aicpu_dispatcher.so" \
            --aicpu "${ROOT}/build/onboard/aicpu/libaicpu_kernel.so" \
            --aicore "${ROOT}/build/onboard/aicore/aicore_kernel.o"
    fi

    if [[ "$SWIMLANE_FLAG" -ne 0 ]]; then
        local raw="${ROOT}/l2_swimlane_records.json"
        if [[ ! -f "$raw" ]]; then
            echo "ERROR: missing $raw for ${case}" >&2
            return 1
        fi

        # Move raw + emit Perfetto trace into out_dir (or ROOT if single-case).
        local name="${case%.h}"
        local trace_out
        if [[ -n "$out_dir" ]]; then
            cp -f "$raw" "${out_dir}/l2_swimlane_records.json"
            trace_out="${out_dir}/l2_swimlane_trace.json"
        else
            trace_out="${ROOT}/l2_swimlane_trace.json"
        fi

        local perfetto_args=("${ROOT}/tools/swimlane_trace.py" "$raw" -o "$trace_out" --case "$name")
        local fn_json
        fn_json="$(resolve_func_names_json "$case")"
        if [[ -n "$fn_json" ]]; then
            perfetto_args+=(--func-names "$fn_json")
        fi
        if [[ "$NO_SUMMARY" -ne 0 ]]; then
            perfetto_args+=(--no-summary)
        fi
        python3 "${perfetto_args[@]}"

        if [[ -n "$out_dir" && -f "$log" ]]; then
            rg "\[scheduler\]|\[orchestration\]|PASS|FAIL|task_cnt|duration|task_tp|subtask_cnt" "$log" >> "${out_dir}/../perf_summary.txt" || true
        fi
        echo "[esl_proxy] OK ${name}: trace=${trace_out}"
    fi
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
    if [[ "$DOUBLE_BUFFER" == "ON" ]]; then
        DISPATCH_LABEL="double_buffer"
    else
        DISPATCH_LABEL="basic"
    fi
    OUT_ROOT="${ROOT}/report/swimlane/${DISPATCH_LABEL}"
    SUMMARY="${OUT_ROOT}/perf_summary.txt"
    mkdir -p "$OUT_ROOT"
    : > "$SUMMARY"
    echo "[esl_proxy] swimlane output: ${OUT_ROOT}/<case>/"

    for case in "${ALL_CASES[@]}"; do
        name="${case%.h}"
        echo "======== CASE ${case} ========" | tee -a "$SUMMARY"
        out_dir="${OUT_ROOT}/${name}"
        mkdir -p "$out_dir"
        export ORCH_CASE="$case"
        export ESL_PROXY_ORCH_CASE="$case"
        build_aicpu
        if ! run_one_case "$case" "$out_dir"; then
            echo "ERROR: case ${case} failed" >&2
            exit 1
        fi
        if [[ "$COOLDOWN" -gt 0 ]]; then
            echo "Waiting ${COOLDOWN}s for device cooldown..." | tee -a "$SUMMARY"
            sleep "$COOLDOWN"
        fi
    done
    echo "ALL DONE"
    cat "$SUMMARY"
else
    single_case="${CASE_OVERRIDE:-${ESL_PROXY_ORCH_CASE:-$DEFAULT_CASE}}"
    run_one_case "$single_case"
fi
