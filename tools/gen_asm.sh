#!/usr/bin/env bash
# Generate .s assembly for esl_proxy sim (host gcc -S) and onboard AICore (ccec .o → .s).
#
# Usage:
#   source $ASCEND_HOME_PATH/bin/setenv.bash
#   bash tools/gen_asm.sh              # sim + aicore asm
#   bash tools/gen_asm.sh --sim-only   # host sim only
#   bash tools/gen_asm.sh --aicore-only
#
# Output: build/asm/{sim,aicore}/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESL_CORE="${ROOT}/esl_proxy"
OUT="${ROOT}/build/asm"
SIM_OUT="${OUT}/sim"
AICORE_OUT="${OUT}/aicore"

DO_SIM=1
DO_AICORE=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --sim-only) DO_AICORE=0; shift ;;
    --aicore-only) DO_SIM=0; shift ;;
    -h|--help)
      echo "usage: $0 [--sim-only|--aicore-only]"
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

# shellcheck disable=SC1091
source "${ROOT}/build/sources.sh"

gen_sim_asm() {
  mkdir -p "$SIM_OUT"
  local CC="${CC:-cc}"
  local CASE="${ORCH_CASE:-qwen3_dynamic_manual_scope.h}"
  local CFLAGS=(
    -S -g -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=199309L
    -DESL_PROXY_AICORE_HOST
    -I"${ESL_CORE}/include/algorithm"
    -I"${ESL_CORE}/include/platform"
    -I"${ESL_CORE}/include/platform/sim"
    -I"${ESL_CORE}/include/platform/onboard"
    -I"${ESL_CORE}/include/swimlane"
    -I"${ESL_CORE}/cases"
    -DORCH_CASE="${CASE}"
  )
  local src
  for src in \
    "${ESL_CORE}/src/algorithm/dispatch_payload.c" \
    "${ESL_CORE}/src/algorithm/dispatch.c" \
    "${ESL_CORE}/src/algorithm/cutter.c"; do
    if [[ ! -f "$src" ]]; then
      echo "[gen_asm] skip missing: $src" >&2
      continue
    fi
    local base
    base="$(basename "${src}" .c)"
    echo "[gen_asm] sim gcc -S: ${base}.s"
    "$CC" "${CFLAGS[@]}" -o "${SIM_OUT}/${base}.s" "$src"
  done
}

gen_aicore_asm() {
  if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
    echo "ASCEND_HOME_PATH required for AICore asm" >&2
    exit 1
  fi

  mkdir -p "$AICORE_OUT"
  local ONBOARD_INC="${ESL_CORE}/include/platform/onboard"
  local BISHENG_CC
  BISHENG_CC="$(command -v ccec || echo "${ASCEND_HOME_PATH}/tools/bisheng_compiler/bin/ccec")"
  local LLVM_OBJDUMP
  LLVM_OBJDUMP="$(command -v llvm-objdump || echo "${ASCEND_HOME_PATH}/tools/bisheng_compiler/bin/llvm-objdump")"
  local INCS=(
    "-I${ONBOARD_INC}"
    "-I${ESL_CORE}/include/platform"
    "-I${ESL_CORE}/include/algorithm"
    "-I${ESL_CORE}/include/swimlane"
  )
  local FLAGS=(
    -c -O3 -g -x cce -Wall --cce-aicore-only
    -DESL_PROXY_AICORE_FAKE_FIN -DESL_PROXY_ENABLE_L2_SWIMLANE=0
    -mllvm -cce-aicore-stack-size=0x8000
    -mllvm -cce-aicore-function-stack-size=0x8000
    -mllvm -cce-aicore-record-overflow=false
    -mllvm -cce-aicore-addr-transform
    -mllvm -cce-aicore-dcci-insert-for-scalar=false
  )

  compile_one() {
    local src="$1"
    local arch="$2"
    local tag="$3"
    local base obj
    base="$(basename "${src}")"
    base="${base%.*}_${tag}"
    obj="${AICORE_OUT}/${base}.o"
    echo "[gen_asm] ccec ${arch}: ${base}.o → .s"
    "$BISHENG_CC" "${FLAGS[@]}" --cce-aicore-arch="${arch}" "${INCS[@]}" -o "$obj" "$src"
    python3 "${ROOT}/tools/obj_to_gas.py" "$obj" \
      -o "${AICORE_OUT}/${base}.s" \
      --llvm-objdump "$LLVM_OBJDUMP" --also-disasm
  }

  compile_one "${ESL_CORE}/src/algorithm/aicore_executor.c" "dav-c220-cube" "aic"
  compile_one "${ESL_CORE}/src/algorithm/aicore_executor.c" "dav-c220-vec" "aiv"
  compile_one "${ESL_CORE}/src/platform/onboard/aicore_entry.cpp" "dav-c220-cube" "aic"
  compile_one "${ESL_CORE}/src/platform/onboard/aicore_entry.cpp" "dav-c220-vec" "aiv"
}

mkdir -p "$OUT"
if [[ "$DO_SIM" -eq 1 ]]; then
  gen_sim_asm
fi
if [[ "$DO_AICORE" -eq 1 ]]; then
  gen_aicore_asm
fi

echo "[gen_asm] done → ${OUT}"
