#!/usr/bin/env bash
# Build and run all four ORCH cases on NPU with L2 swimlane (level 1),
# export raw + Perfetto trace JSON under report/swimlane/<case>/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CASES=(
    qwen3_dynamic_manual_scope.h
    qwen3_dynamic_tensormap.h
    paged_attention_unroll.h
    paged_attention_unroll_manual_scope.h
)

OUT_ROOT="${ROOT}/report/swimlane"
mkdir -p "$OUT_ROOT"

export ESL_PROXY_L2_SWIMLANE_LEVEL=1
export ESL_PROXY_ENABLE_L2_SWIMLANE=1

resolve_func_names() {
    local case="$1"
    case "$case" in
        paged_attention_*) echo "${ROOT}/tools/paged_attention_func_names.json" ;;
        qwen3_*) echo "${ROOT}/tools/qwen3_func_names.json" ;;
        *) echo "" ;;
    esac
}

for case in "${CASES[@]}"; do
    name="${case%.h}"
    out_dir="${OUT_ROOT}/${name}"
    mkdir -p "$out_dir"

    echo "========================================"
    echo "[esl_proxy] onboard swimlane case=${case}"
    echo "========================================"

    export ESL_PROXY_ORCH_CASE="$case"
    rm -f "${ROOT}/l2_swimlane_records.json" "${ROOT}/l2_swimlane_trace.json"

    bash "${ROOT}/tools/run_onboard.sh"

    if [[ ! -f "${ROOT}/l2_swimlane_records.json" ]]; then
        echo "ERROR: missing l2_swimlane_records.json for ${case}" >&2
        exit 1
    fi

    cp -f "${ROOT}/l2_swimlane_records.json" "${out_dir}/l2_swimlane_records.json"

    fn_json="$(resolve_func_names "$case")"
    perfetto_args=("${ROOT}/tools/swimlane_to_perfetto.py" "${out_dir}/l2_swimlane_records.json" -o "${out_dir}/l2_swimlane_trace.json")
    if [[ -n "$fn_json" ]]; then
        perfetto_args+=(--func-names "$fn_json")
    fi
    python3 "${perfetto_args[@]}"

    if [[ ! -f "${out_dir}/l2_swimlane_trace.json" ]]; then
        echo "ERROR: Perfetto conversion failed for ${case}" >&2
        exit 1
    fi

    echo "[esl_proxy] OK ${name}:"
    echo "  raw:     ${out_dir}/l2_swimlane_records.json"
    echo "  perfetto: ${out_dir}/l2_swimlane_trace.json"
    echo "  open:    https://ui.perfetto.dev/"
    echo
done

echo "All cases finished. Traces under ${OUT_ROOT}/"
