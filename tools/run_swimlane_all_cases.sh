#!/usr/bin/env bash
# Run all four ORCH cases on NPU with L2 swimlane; export traces + perf summary.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export ASCEND_HOME_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/cann-9.0.0}"
export SIMPLER_ROOT="${SIMPLER_ROOT:-$(cd "$ROOT/../simpler" 2>/dev/null && pwd)}"
export ESL_PROXY_L2_SWIMLANE_LEVEL=1
export ESL_PROXY_ENABLE_L2_SWIMLANE=1

CASES=(
    qwen3_dynamic_manual_scope.h
    qwen3_dynamic_tensormap.h
    paged_attention_unroll.h
    paged_attention_unroll_manual_scope.h
)

OUT_ROOT="${ROOT}/report/swimlane"
SUMMARY="${OUT_ROOT}/perf_summary.txt"
mkdir -p "$OUT_ROOT"
: > "$SUMMARY"

DEVICE="${TASK_DEVICE:-0}"

resolve_func_names() {
    case "$1" in
        paged_attention_*) echo "${ROOT}/tools/paged_attention_func_names.json" ;;
        qwen3_*) echo "${ROOT}/tools/qwen3_func_names.json" ;;
        *) echo "" ;;
    esac
}

for case in "${CASES[@]}"; do
    name="${case%.h}"
    out_dir="${OUT_ROOT}/${name}"
    mkdir -p "$out_dir"
    export ESL_PROXY_ORCH_CASE="$case"
    export ORCH_CASE="$case"

    echo "======== CASE ${case} ========"
    echo "======== CASE ${case} ========" >> "$SUMMARY"

    log="${out_dir}/run.log"
    bash "${ROOT}/tools/run_onboard.sh" -d "$DEVICE" 2>&1 | tee "$log"

    if [[ ! -f "${ROOT}/l2_swimlane_records.json" ]]; then
        echo "ERROR: missing l2_swimlane_records.json for ${case}" >&2
        exit 1
    fi
    cp -f "${ROOT}/l2_swimlane_records.json" "${out_dir}/"

    fn_json="$(resolve_func_names "$case")"
    if [[ -n "$fn_json" ]]; then
        python3 "${ROOT}/tools/swimlane_to_perfetto.py" \
            "${out_dir}/l2_swimlane_records.json" \
            -o "${out_dir}/l2_swimlane_trace.json" \
            --func-names "$fn_json"
    else
        python3 "${ROOT}/tools/swimlane_to_perfetto.py" \
            "${out_dir}/l2_swimlane_records.json" \
            -o "${out_dir}/l2_swimlane_trace.json"
    fi

    rg "\[scheduler\]|\[orchestration\]|PASS|FAIL|task_cnt|duration|task_tp|subtask_cnt" "$log" >> "$SUMMARY" || true
    python3 "${ROOT}/tools/swimlane_perf_summary.py" "${out_dir}/l2_swimlane_records.json" "$name" >> "$SUMMARY"
    echo >> "$SUMMARY"
    echo "Waiting 30s for device cooldown..." >> "$SUMMARY"
    sleep 30
done

echo "ALL DONE"
cat "$SUMMARY"
