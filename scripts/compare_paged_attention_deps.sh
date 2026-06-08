#!/usr/bin/env bash
# Compare paged_attention_unroll dependency edges before/after TensorMap PTO2 align.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ESL_DIR="${REPO_ROOT}/esl_proxy"
DEP_DUMP_DIR="${REPO_ROOT}/tools/dep_dump"
CASES_DIR="${ESL_DIR}/include"
OUT_DIR="${REPO_ROOT}/build/pa_dep_compare"
BASE_TAG="${BASE_TAG:-tensormap-pre-pto2-align}"

CC="${CC:-cc}"
CFLAGS=(
  -std=c11 -Wall -Wextra -pedantic -O0
  -D_POSIX_C_SOURCE=200809L
  -I "${ESL_DIR}/include"
  -I "${ESL_DIR}/cases"
  -I "${DEP_DUMP_DIR}"
  -DUSE_TENSORMAP -DDEP_DUMP=1
)

mkdir -p "${OUT_DIR}"

build_test() {
  local tensormap_src="$1"
  local out_bin="$2"
  cp "${tensormap_src}" "${CASES_DIR}/tensormap.h"
  "${CC}" "${CFLAGS[@]}" \
    "${DEP_DUMP_DIR}/tests/test_dep_dump_paged_attention_edges.c" \
    "${DEP_DUMP_DIR}/dep_dump.c" \
    -o "${out_bin}" -lpthread -latomic
}

run_dump() {
  local bin="$1"
  local edge_csv="$2"
  DEP_DUMP_EDGE_FILE="${edge_csv}" "${bin}"
}

echo "=== paged_attention dep edge compare ==="
echo "baseline tag: ${BASE_TAG}"
echo "output dir:   ${OUT_DIR}"

TM_BEFORE="${OUT_DIR}/tensormap_before.h"
TM_AFTER="${ESL_DIR}/include/tensormap.h"
TM_SAVED="${OUT_DIR}/tensormap_current.h"

cp "${TM_AFTER}" "${TM_SAVED}"
git -C "${REPO_ROOT}" show "${BASE_TAG}:esl_proxy/include/tensormap.h" > "${TM_BEFORE}"

BIN_BEFORE="${OUT_DIR}/test_pa_edges_before"
BIN_AFTER="${OUT_DIR}/test_pa_edges_after"
EDGES_BEFORE="${OUT_DIR}/edges_before.csv"
EDGES_AFTER="${OUT_DIR}/edges_after.csv"

echo "--- build BEFORE (${BASE_TAG}) ---"
build_test "${TM_BEFORE}" "${BIN_BEFORE}"
echo "--- build AFTER (current working tree) ---"
build_test "${TM_SAVED}" "${BIN_AFTER}"

cp "${TM_SAVED}" "${CASES_DIR}/tensormap.h"

echo "--- run BEFORE ---"
run_dump "${BIN_BEFORE}" "${EDGES_BEFORE}" | tee "${OUT_DIR}/run_before.log"
echo "--- run AFTER ---"
run_dump "${BIN_AFTER}" "${EDGES_AFTER}" | tee "${OUT_DIR}/run_after.log"

BEFORE_CNT=$(tail -n +2 "${EDGES_BEFORE}" | wc -l | tr -d ' ')
AFTER_CNT=$(tail -n +2 "${EDGES_AFTER}" | wc -l | tr -d ' ')

echo ""
echo "edge count: before=${BEFORE_CNT} after=${AFTER_CNT}"

if diff -u "${EDGES_BEFORE}" "${EDGES_AFTER}" > "${OUT_DIR}/edges.diff"; then
  echo "RESULT: dependency edges UNCHANGED (sorted CSV identical)"
  rm -f "${OUT_DIR}/edges.diff"
  exit 0
else
  echo "RESULT: dependency edges CHANGED — see ${OUT_DIR}/edges.diff"
  head -40 "${OUT_DIR}/edges.diff" || true
  exit 1
fi
