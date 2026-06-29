#!/usr/bin/env bash
# Sim benchmark: all cases, 10 runs each, median metrics. Writes JSON + markdown snippet.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESL="${ROOT}/esl_proxy"
REPORT_DIR="${ROOT}/report"
RUNS="${RUNS:-10}"
SPMD="${QWEN3_SPMD_TIER:-0}"
SIM_AICORE="${SIM_AICORE:-instant}"
DISPATCH="${DISPATCH:-basic}"

mkdir -p "$REPORT_DIR"

python3 - "$ESL" "$REPORT_DIR" "$RUNS" "$SPMD" "$SIM_AICORE" "$DISPATCH" <<'PY'
import json, re, statistics, subprocess, sys, time
from datetime import datetime, timezone

ESL, REPORT_DIR, RUNS, SPMD, SIM_AICORE, DISPATCH = sys.argv[1:7]
RUNS = int(RUNS)
CASES = [
    "qwen3_dynamic_manual_scope.h",
    "qwen3_dynamic_tensormap.h",
    "paged_attention_unroll.h",
    "paged_attention_unroll_manual_scope.h",
]
PAT = {
    "task_cnt": r"\[orchestration\] task_cnt = (\d+)",
    "subtask_cnt": r"\[orchestration\] subtask_cnt = (\d+)",
    "orch_elapsed_ns": r"\[orchestration\] elapsed_time = (\d+) ns",
    "orch_task_tp": r"\[orchestration\] task_tp = ([0-9.]+) MTasks/s",
    "sched_duration_ns": r"\[scheduler\] duration = (\d+) ns",
    "sched_task_tp": r"\[scheduler\] task_tp = ([0-9.]+) MTasks/s",
}

def parse(out):
    m = {}
    for k, p in PAT.items():
        mm = re.search(p, out)
        if not mm:
            raise RuntimeError(f"missing {k}")
        m[k] = float(mm.group(1)) if "." in mm.group(1) else int(mm.group(1))
    if "PASS" not in out:
        raise RuntimeError("no PASS")
    return m

def median(vals):
    return statistics.median(vals)

results = {
    "timestamp": datetime.now(timezone.utc).isoformat(),
    "mode": "sim",
    "sim_aicore": SIM_AICORE,
    "dispatch": DISPATCH,
    "qwen3_spmd_tier": int(SPMD),
    "runs_per_case": RUNS,
    "cases": {},
}

for case in CASES:
    print(f"[sim] build {case}", flush=True)
    subprocess.run(["make", "clean"], cwd=ESL, check=True, capture_output=True)
    mk = [
        "make", "all", f"CASE={case}", "MAIN_LOG=1",
        f"QWEN3_SPMD_TIER={SPMD}", f"SIM_AICORE={SIM_AICORE}",
        f"DISPATCH={DISPATCH}",
    ]
    subprocess.run(mk, cwd=ESL, check=True, capture_output=True)
    samples = []
    for i in range(RUNS):
        t0 = time.perf_counter()
        rr = subprocess.run(["./bin/esl_proxy"], cwd=ESL, capture_output=True, text=True)
        wall_ms = (time.perf_counter() - t0) * 1000
        out = rr.stdout + rr.stderr
        if rr.returncode != 0:
            raise RuntimeError(f"{case} run {i+1} failed:\n{out[-800:]}")
        s = parse(out)
        s["wall_ms"] = wall_ms
        samples.append(s)
        print(f"  run {i+1}/{RUNS}: sched_tp={s['sched_task_tp']:.3f}", flush=True)
    med = {k: median([s[k] for s in samples]) for k in samples[0] if k != "wall_ms"}
    med["wall_ms"] = median([s["wall_ms"] for s in samples])
    med["pass"] = RUNS
    results["cases"][case] = med

out_json = f"{REPORT_DIR}/sim_benchmark.json"
with open(out_json, "w") as f:
    json.dump(results, f, indent=2)
print(f"Wrote {out_json}")
PY
