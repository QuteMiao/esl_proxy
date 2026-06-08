#!/usr/bin/env bash
# Benchmark esl_proxy tensormap orchestration cases.
# Usage: ./scripts/bench_tensormap_cases.sh [section_label]
#   section_label — appended to report (e.g. Baseline, After-Phase3)
# Env: BENCH_RUNS (default 10), ESL_PROXY_DIR (default esl_proxy subdir)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# Makefile lives in ${REPO_ROOT}/esl_proxy when scripts/ is at repo root.
ESL_DIR="${ESL_PROXY_DIR:-${REPO_ROOT}/esl_proxy}"
REPORT="${REPO_ROOT}/report/proxy/tensormap_alignment_benchmark.md"
RUNS="${BENCH_RUNS:-10}"
SECTION="${1:-Run}"

mkdir -p "$(dirname "${REPORT}")"

median_of() {
  awk '{
    n=NF; for(i=1;i<=n;i++) a[i]=$i
    for(i=1;i<=n;i++) for(j=i+1;j<=n;j++) if(a[i]>a[j]) {t=a[i];a[i]=a[j];a[j]=t}
    if(n%2) print a[(n+1)/2]; else print (a[n/2]+a[n/2+1])/2
  }'
}

run_case() {
  local case_name="$1"
  local tier_arg="${2:-}"
  local elapsed_list="" task_tp_list="" subtask_tp_list="" pool_hw=""
  local i elapsed task_tp subtask_tp

  for ((i = 1; i <= RUNS; i++)); do
    local out
    if [[ -n "${tier_arg}" ]]; then
      out="$(cd "${ESL_DIR}" && CCACHE_DISABLE=1 make -s CASE="${case_name}" "${tier_arg}" run 2>&1)"
    else
      out="$(cd "${ESL_DIR}" && CCACHE_DISABLE=1 make -s CASE="${case_name}" run 2>&1)"
    fi
    elapsed="$(echo "${out}" | sed -n 's/.*\[orchestration\] elapsed_time = \([0-9]*\) ns.*/\1/p' | head -1)"
    task_tp="$(echo "${out}" | sed -n 's/.*\[orchestration\] task_tp = \([0-9.]*\) MTasks\/s.*/\1/p' | head -1)"
    subtask_tp="$(echo "${out}" | sed -n 's/.*\[orchestration\] subtask_tp = \([0-9.]*\) MTasks\/s.*/\1/p' | head -1)"
    pool_hw="$(echo "${out}" | sed -n 's/.*\[tensormap\] pool_high_water=\([0-9]*\).*/\1/p' | head -1)"
    [[ -n "${elapsed}" ]] || { echo "FAIL: no elapsed_time for ${case_name} ${tier_arg}" >&2; echo "${out}" >&2; return 1; }
    elapsed_list="${elapsed_list} ${elapsed}"
    task_tp_list="${task_tp_list} ${task_tp}"
    subtask_tp_list="${subtask_tp_list} ${subtask_tp}"
  done

  local med_elapsed med_task_tp med_subtask_tp min_elapsed max_elapsed
  med_elapsed="$(echo "${elapsed_list}" | median_of)"
  med_task_tp="$(echo "${task_tp_list}" | median_of)"
  med_subtask_tp="$(echo "${subtask_tp_list}" | median_of)"
  min_elapsed="$(echo "${elapsed_list}" | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n | head -1)"
  max_elapsed="$(echo "${elapsed_list}" | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n | tail -1)"

  local tier_label="${tier_arg:-default}"
  tier_label="${tier_label//QWEN3_SPMD_TIER=/tier=}"

  {
    echo "| ${case_name} | ${tier_label} | ${RUNS} | ${med_elapsed} | ${min_elapsed}-${max_elapsed} | ${med_task_tp} | ${med_subtask_tp} | ${pool_hw:-n/a} |"
  } >> "${REPORT}.tmp"
}

if [[ ! -f "${REPORT}" ]] || ! grep -q "^# TensorMap Alignment Benchmark" "${REPORT}" 2>/dev/null; then
  cat > "${REPORT}" <<'EOF'
# TensorMap Alignment Benchmark

Orchestration-only timing (`ORCHESTRATION_TIME=1`, scheduler not started).

EOF
fi

{
  echo ""
  echo "## ${SECTION}"
  echo ""
  echo "Runs per case: ${RUNS}"
  echo ""
  echo "| Case | Variant | Runs | elapsed_ns (median) | min-max ns | task_tp MTasks/s | subtask_tp MTasks/s | pool_high_water |"
  echo "|------|---------|------|---------------------|------------|------------------|---------------------|-----------------|"
} > "${REPORT}.tmp"

run_case "qwen3_dynamic_tensormap.h" "QWEN3_SPMD_TIER=0"
run_case "qwen3_dynamic_tensormap.h" "QWEN3_SPMD_TIER=2"
run_case "qwen3_dynamic_tensormap.h" "QWEN3_SPMD_TIER=4"
run_case "paged_attention_unroll.h" ""

cat "${REPORT}.tmp" >> "${REPORT}"
rm -f "${REPORT}.tmp"
echo "Appended section '${SECTION}' to ${REPORT}"
