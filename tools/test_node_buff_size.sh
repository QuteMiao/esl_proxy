#!/usr/bin/env bash
# Sweep NODE_BUFF_SIZE for qwen3 sim + onboard.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONF="${ROOT}/esl_proxy/include/algorithm/conf.h"
MAKE_DIR="${ROOT}/esl_proxy"
CASES=(
    qwen3_dynamic_manual_scope.h
    qwen3_dynamic_tensormap.h
)
SIZES=(8192 16384 32768 65536)

ORIG_SIZE=$(grep '^#define NODE_BUFF_SIZE' "$CONF" | awk '{print $3}')

set_node_buff_size() {
    sed -i "s/^#define NODE_BUFF_SIZE .*/#define NODE_BUFF_SIZE $1/" "$CONF"
    echo ">>> NODE_BUFF_SIZE=$1"
}

run_sim_case() {
    local case="$1"
    local log
    log="$(mktemp)"
    if (cd "$MAKE_DIR" && make clean >/dev/null 2>&1 && \
        make -j"$(nproc)" CASE="$case" QWEN3_SPMD_TIER=0 >/dev/null 2>&1 && \
        ./bin/esl_proxy >"$log" 2>&1); then
        if grep -q '\[host\] PASS' "$log"; then
            local tasks
            tasks=$(grep -o 'task_cnt=[0-9]*' "$log" | tail -1)
            echo "  sim PASS  $case  ($tasks)"
            rm -f "$log"
            return 0
        fi
    fi
    echo "  sim FAIL  $case"
    grep -E 'FAIL|SEGV|Aborted|Error|task_cnt' "$log" 2>/dev/null | tail -3 || tail -3 "$log"
    rm -f "$log"
    return 1
}

run_onboard_case() {
    local case="$1"
    local log
    log="$(mktemp)"
    if task-submit --timeout 180 --max-time 180 --device auto --device-num 1 \
        --run "cd ${ROOT} && QWEN3_SPMD_TIER=0 bash tools/run_onboard.sh --case ${case} --npu" \
        >"$log" 2>&1; then
        if grep -q 'task_cnt=3096' "$log" && \
           ! grep -qiE 'FAIL|SEGV|Aborted|507018|sync after aicpu exec failed' "$log"; then
            local wall
            wall=$(grep -o 'wall_ns=[0-9]*' "$log" | tail -1)
            echo "  onboard PASS  $case  ($wall)"
            rm -f "$log"
            return 0
        fi
    fi
    echo "  onboard FAIL  $case"
    grep -iE 'FAIL|SEGV|Aborted|507018|sync after|task_cnt=' "$log" 2>/dev/null | tail -5 || tail -5 "$log"
    rm -f "$log"
    return 1
}

restore_conf() {
    sed -i "s/^#define NODE_BUFF_SIZE .*/#define NODE_BUFF_SIZE ${ORIG_SIZE}/" "$CONF"
}
trap restore_conf EXIT

echo "Original NODE_BUFF_SIZE=${ORIG_SIZE}"
echo "========================================"

RESULTS_FILE="${ROOT}/report/node_buff_sweep.txt"
mkdir -p "${ROOT}/report"
: > "$RESULTS_FILE"

for sz in "${SIZES[@]}"; do
    set_node_buff_size "$sz"
    sim_ok=1
    onboard_ok=1

    echo "--- Sim (NODE_BUFF_SIZE=${sz}) ---"
    for c in "${CASES[@]}"; do
        if ! run_sim_case "$c"; then sim_ok=0; fi
    done

    echo "--- Onboard (NODE_BUFF_SIZE=${sz}) ---"
    for c in "${CASES[@]}"; do
        if ! run_onboard_case "$c"; then onboard_ok=0; fi
    done

    if [[ "$sim_ok" -eq 1 && "$onboard_ok" -eq 1 ]]; then
        verdict="ALL PASS"
    elif [[ "$sim_ok" -eq 1 ]]; then
        verdict="sim PASS / onboard FAIL"
    elif [[ "$onboard_ok" -eq 1 ]]; then
        verdict="sim FAIL / onboard PASS"
    else
        verdict="ALL FAIL"
    fi
    echo ">>> ${sz}: ${verdict}"
    echo "${sz} ${verdict}" >> "$RESULTS_FILE"
    echo "========================================"
done

echo "Results written to ${RESULTS_FILE}"
cat "$RESULTS_FILE"
