#!/usr/bin/env python3
"""Generate tools/case_task_func_maps.json — the fixed task_id -> func_id maps
used by swimlane_trace.py to label L2 swimlane records.

The mapping is deterministic per case (emission order is fixed). For qwen3 cases
it ALSO depends on the SPMD tier (QWEN3_SPMD_TIER 0..4 changes how many tasks each
kernel emits), so we capture one map per tier. paged_attention cases are not SPMD
(count=1) → a single flat map.

Method: build+run the case in sim with WORKER_LOG=1 and read log/pto._thread_0.csv.
Each `new,task_id,<id>,...,dur,<d>` line gives the stored (uint16) duration, which
is UNIQUE per kernel; the first-appearance order of durations is exactly the kernel
emission order = the func_names order (0,1,2,...). So func_id = index at which a
duration first appears. No hard-coded kernel table needed.

Usage: python3 tools/gen_case_task_func_maps.py   (run from repo root; needs a sim toolchain)
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "esl_proxy"  # nested dir with the Makefile

QWEN3_CASES = ["qwen3_dynamic_manual_scope", "qwen3_dynamic_tensormap"]
PAGED_CASES = ["paged_attention_unroll", "paged_attention_unroll_manual_scope"]
SPMD_TIERS = [0, 1, 2, 3, 4]

_NEW_RE = re.compile(r"new,task_id,(\d+),type,\d+,subtask_cnt,\d+,dur,(\d+)")


def _run_case(case: str, tier: int | None) -> dict[str, int]:
    """Build+run the case in sim, return {task_id(str): func_id}."""
    env_tier = [] if tier is None else [f"QWEN3_SPMD_TIER={tier}"]
    make = ["make", f"CASE={case}.h", "WORKER_LOG=1", *env_tier]
    subprocess.run([*make, "clean"], cwd=CORE, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([*make, "all"], cwd=CORE, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    logdir = CORE / "log"
    for f in logdir.glob("*.csv"):
        f.unlink()
    # sim hangs at shutdown after work completes; timeout + WORKER_LOG env.
    subprocess.run(["stdbuf", "-oL", "timeout", "20", "./bin/esl_proxy"],
                   cwd=CORE, env={**_env(), "WORKER_LOG": "1"},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # parse the orchestration thread log (thread_0 emits the 'new' lines)
    dur_to_fid: dict[str, int] = {}
    task_map: dict[str, int] = {}
    text = ""
    for f in sorted(logdir.glob("*.csv")):
        text += f.read_text()
    for tid, dur in _NEW_RE.findall(text):
        if dur not in dur_to_fid:
            dur_to_fid[dur] = len(dur_to_fid)
        task_map[tid] = dur_to_fid[dur]
    return dict(sorted(task_map.items(), key=lambda kv: int(kv[0])))


def _env() -> dict:
    import os
    return dict(os.environ)


def main() -> int:
    out: dict = {
        "_note": "task_id -> func_id per case, for L2 swimlane labeling "
                 "(tools/swimlane_trace.py). func_id indexes <family>_func_names.json "
                 "(= kernel emission order). qwen3_* are keyed by QWEN3_SPMD_TIER (0..4) "
                 "because the SPMD tier changes per-kernel task counts; paged_* are flat "
                 "(not SPMD). Regenerate with tools/gen_case_task_func_maps.py."
    }
    for case in QWEN3_CASES:
        per_tier = {}
        for t in SPMD_TIERS:
            m = _run_case(case, t)
            per_tier[str(t)] = m
            print(f"{case} tier {t}: {len(m)} tasks, func_ids {sorted(set(m.values()))}",
                  file=sys.stderr)
        out[case] = per_tier
    for case in PAGED_CASES:
        m = _run_case(case, None)
        out[case] = m
        print(f"{case} (flat): {len(m)} tasks, func_ids {sorted(set(m.values()))}",
              file=sys.stderr)

    dst = ROOT / "tools" / "case_task_func_maps.json"
    dst.write_text(json.dumps(out, separators=(",", ":")))
    print(f"wrote {dst} ({dst.stat().st_size} bytes)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
