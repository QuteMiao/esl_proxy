#!/usr/bin/env bash
# Generate DAG .dot/.svg for every orchestration case.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ESL="$ROOT/esl_proxy"
OUT="$ESL/report/dag"
CASES=(
    qwen3_dynamic_manual_scope.h
    qwen3_dynamic_tensormap.h
    paged_attention_unroll.h
    paged_attention_unroll_manual_scope.h
)

mkdir -p "$OUT"

for case in "${CASES[@]}"; do
    name="${case%.h}"
    echo "=== DAG: $case ==="
    make -C "$ESL" clean
    make -C "$ESL" \
        CASE="$case" \
        MAIN_LOG=0 \
        EXTRA_CFLAGS="-DMAIN_LOG=0 -DLOG_OUTPUT_MODE=0"
    rm -f "$ESL/log/pto._thread_"*.csv
    (cd "$ESL" && WORKER_LOG=1 LOG_OUTPUT_MODE=0 ./bin/esl_proxy)
    cp "$ESL/log/pto._thread_0.csv" "$OUT/${name}.csv"
    python3 "$ROOT/tools/gen_dag.py" "$OUT/${name}.csv" -o "$OUT"
    echo
done

echo "All DAGs written to $OUT"
