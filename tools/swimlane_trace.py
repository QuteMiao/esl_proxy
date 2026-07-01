#!/usr/bin/env python3
"""esl_proxy swimlane trace tool: convert l2_swimlane_records.json to Perfetto
Chrome-trace JSON, and print a one-line performance summary.

This merges the former swimlane_to_perfetto.py (Perfetto conversion) and
swimlane_perf_summary.py (perf summary) into a single entry point.

Default behaviour:
    python3 tools/swimlane_trace.py <records.json> [-o <trace.json>]
        -> writes Perfetto trace JSON + prints perf summary.

    python3 tools/swimlane_trace.py <records.json> --summary-only [<case_name>]
        -> prints perf summary only (no trace JSON).

Raw swimlane dumps are an intermediate schema emitted by the C collector
(host_swimlane.c -> collector_export_swimlane_json); Perfetto
(https://ui.perfetto.dev/) expects {"traceEvents": [...]} Chrome trace format.
This script reuses simpler's swimlane_converter (v2 reader + trace emitter).
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import sys
from pathlib import Path

# Perfetto / Chrome trace cname palette (one color per kernel name).
_KERNEL_COLORS = {
    "qk_matmul": "good",
    "softmax_prepare": "bad",
    "pv_matmul": "olive",
    "online_update": "rail_response",
    "rmsnorm": "good",
    "q_proj": "olive",
    "k_proj": "rail_response",
    "v_proj": "rail_idle",
    "qk_norm": "terrible",
    "rope_kv_cache": "rail_load",
    "softmax": "bad",
    "sv_matmul": "olive",
    "online_softmax": "rail_response",
    "out_proj": "crimson",
    "post_rmsnorm": "good",
    "gate_proj": "bad",
    "up_proj": "olive",
    "silu": "rail_response",
    "down_proj": "rail_load",
    "down_proj_residual": "terrible",
}


# ---------------------------------------------------------------------------
# swimlane_converter loader (from simpler checkout)
# ---------------------------------------------------------------------------

def _find_simpler_root(esl_root: Path) -> Path:
    env = os.environ.get("SIMPLER_ROOT", "").strip()
    if env:
        root = Path(env).resolve()
        if (root / "simpler_setup" / "tools" / "swimlane_converter.py").is_file():
            return root

    for base in [esl_root, *list(esl_root.parents)[:5]]:
        for rel in ("simpler", "vendor/simpler"):
            root = (base / rel).resolve()
            if (root / "simpler_setup" / "tools" / "swimlane_converter.py").is_file():
                return root

    raise FileNotFoundError(
        "swimlane_converter not found; set SIMPLER_ROOT to the simpler checkout "
        "(expected simpler_setup/tools/swimlane_converter.py)"
    )


def _load_swimlane_converter():
    esl_root = Path(__file__).resolve().parents[1]
    simpler_root = _find_simpler_root(esl_root)
    module_path = simpler_root / "simpler_setup" / "tools" / "swimlane_converter.py"

    spec = importlib.util.spec_from_file_location("esl_swimlane_converter", module_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot load module spec from {module_path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _load_func_names(path: Path | None, esl_root: Path) -> dict[str, str]:
    if path is not None and path.is_file():
        with open(path) as f:
            raw = json.load(f)
        return {str(k): str(v) for k, v in raw.items()}

    default_pa = esl_root / "tools" / "paged_attention_func_names.json"
    if default_pa.is_file():
        with open(default_pa) as f:
            raw = json.load(f)
        return {str(k): str(v) for k, v in raw.items()}
    return {}


def _load_task_func_map(esl_root: Path, case: str | None, spmd_tier: int = 0) -> dict[str, int]:
    """Load the fixed task_id -> func_id map for `case` from
    tools/case_task_func_maps.json (one merged file, keyed by case name).

    qwen3_* entries are keyed by SPMD tier (values are per-tier dicts) because the
    tier changes per-kernel task counts; paged_* entries are a flat task_id->func_id
    dict. Regenerate the JSON with tools/gen_case_task_func_maps.py.
    """
    if not case:
        return {}
    p = esl_root / "tools" / "case_task_func_maps.json"
    if not p.is_file():
        return {}
    with open(p) as f:
        allmaps = json.load(f)
    key = case[:-2] if case.endswith(".h") else case
    entry = allmaps.get(key) or allmaps.get(case) or {}
    if not entry:
        return {}
    # per-tier (values are dicts) vs flat (values are ints)
    if isinstance(next(iter(entry.values())), dict):
        entry = entry.get(str(spmd_tier)) or entry.get("0") or {}
    return {str(k): int(v) for k, v in entry.items()}


def _resolve_func_ids(tasks: list, task_func_map: dict[str, int]) -> None:
    """Assign each task's func_id from the per-case task_id->func_id map.

    task_id is the orchestration id carried verbatim in the swimlane token
    (task_token_raw = desc->id). Tasks absent from the map keep func_id=-1
    (rendered as task(rXtY) by the converter).
    """
    if not task_func_map:
        return
    for task in tasks:
        tid = str(int(task.get("task_id", -1)))
        if tid in task_func_map:
            task["func_id"] = task_func_map[tid]


def _filter_aicore_view_only(events: list) -> list:
    """Keep only Worker View (pid=4): AICore kernel start->end slices and lane metadata."""
    kept = [e for e in events if e.get("pid") == 4]
    for e in kept:
        if e.get("cat") == "__metadata" and e.get("name") == "process_sort_index":
            e["args"] = {"sort_index": 1}
    return kept


def _apply_kernel_colors(events: list) -> None:
    """Assign Perfetto cname per kernel so different tasks render in distinct colors."""
    for e in events:
        if e.get("ph") != "X" or e.get("cat") != "event":
            continue
        name = e.get("name", "")
        kernel = name.split("(")[0]
        color = _KERNEL_COLORS.get(kernel)
        if color:
            e["cname"] = color


# ---------------------------------------------------------------------------
# perf summary (merged from swimlane_perf_summary.py)
# ---------------------------------------------------------------------------

def _extract_durations(data: dict) -> tuple[list[int], list[int], list[int]]:
    tasks = data.get("aicore_tasks", [])
    durs: list[int] = []
    starts: list[int] = []
    ends: list[int] = []
    for row in tasks:
        if isinstance(row, (list, tuple)) and len(row) >= 5:
            st, en = int(row[3]), int(row[4])
            starts.append(st)
            ends.append(en)
            if en >= st:
                durs.append(en - st)
        elif isinstance(row, dict):
            st, en = row.get("start_time"), row.get("end_time")
            if st is not None and en is not None:
                st, en = int(st), int(en)
                starts.append(st)
                ends.append(en)
                if en >= st:
                    durs.append(en - st)
    return durs, starts, ends


def print_perf_summary(records_path: Path, case: str | None = None) -> int:
    """Print a one-line perf summary; returns 0 on success, 1 if no durations."""
    data = json.loads(records_path.read_text())
    freq = int(data.get("metadata", {}).get("clock_freq_hz", 50_000_000))
    durs, starts, ends = _extract_durations(data)
    if not durs:
        return 1

    span = max(ends) - min(starts)
    total = sum(durs)
    n = len(durs)
    label = case if case else records_path.stem
    print(
        f"swimlane: case={label} tasks={n} cores={data.get('metadata', {}).get('num_cores')} "
        f"span_ms={span / freq * 1000:.2f} avg_task_us={total / n / freq * 1e6:.1f} "
        f"ktasks_per_s={n / (span / freq) / 1000:.2f}"
    )
    return 0


# ---------------------------------------------------------------------------
# Perfetto trace conversion
# ---------------------------------------------------------------------------

def convert_to_perfetto(
    input_path: Path,
    output_path: Path,
    func_names_path: Path | None,
    all_views: bool,
    verbose: bool,
    case: str | None = None,
    spmd_tier: int = 0,
) -> int:
    esl_root = Path(__file__).resolve().parents[1]
    func_names = _load_func_names(func_names_path, esl_root)
    task_func_map = _load_task_func_map(esl_root, case, spmd_tier)

    conv = _load_swimlane_converter()
    data = conv.read_perf_data(input_path)
    _resolve_func_ids(data["tasks"], task_func_map)
    conv.generate_chrome_trace_json(
        data["tasks"],
        str(output_path),
        func_names,
        verbose,
        scheduler_phases=data.get("aicpu_scheduler_phases"),
        orchestrator_phases=data.get("aicpu_orchestrator_phases"),
        core_to_thread=data.get("core_to_thread"),
        deps_edges=None,
        deps_kernel_map=None,
        emit_overhead=False,
    )

    with open(output_path) as f:
        trace = json.load(f)
    if "traceEvents" not in trace:
        print(f"Error: output missing traceEvents wrapper", file=sys.stderr)
        return 1

    if not all_views:
        trace["traceEvents"] = _filter_aicore_view_only(trace["traceEvents"])
    _apply_kernel_colors(trace["traceEvents"])

    with open(output_path, "w") as f:
        json.dump(trace, f, indent=2)

    n = len(trace["traceEvents"])
    view_note = "AICore View only" if not all_views else "all views"
    names_note = f", {len(func_names)} kernel names" if func_names else ""
    print(f"Perfetto trace written: {output_path} ({n} events, {view_note}{names_note})")
    print("Open https://ui.perfetto.dev/ and drag the file to visualize.")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert l2_swimlane_records.json to Perfetto JSON + print perf summary"
    )
    parser.add_argument(
        "input",
        nargs="?",
        default="l2_swimlane_records.json",
        help="Input raw swimlane JSON (default: ./l2_swimlane_records.json)",
    )
    parser.add_argument(
        "-o",
        "--output",
        help="Output Perfetto JSON (default: <input_dir>/l2_swimlane_trace.json)",
    )
    parser.add_argument(
        "--func-names",
        help="JSON map func_id (string) -> kernel name (default: tools/paged_attention_func_names.json)",
    )
    parser.add_argument(
        "--all-views",
        action="store_true",
        help="Also emit AICPU View / phase tracks (default: AICore View only)",
    )
    parser.add_argument(
        "--summary-only",
        action="store_true",
        help="Only print perf summary (skip Perfetto conversion). A case name may be passed via --case.",
    )
    parser.add_argument(
        "--case",
        default=None,
        help="Case name used in the perf summary label (default: input file stem).",
    )
    parser.add_argument(
        "--spmd-tier",
        type=int,
        default=0,
        help="QWEN3_SPMD_TIER (0..4) the run was built with; selects the qwen3 "
             "task_id->func_id map (ignored for non-SPMD cases).",
    )
    parser.add_argument(
        "--no-summary",
        action="store_true",
        help="Skip printing the perf summary (only meaningful without --summary-only).",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    input_path = Path(args.input).resolve()
    if not input_path.is_file():
        print(f"Error: input not found: {input_path}", file=sys.stderr)
        return 1

    # Summary-only mode: no trace conversion.
    if args.summary_only:
        return print_perf_summary(input_path, args.case)

    output_path = (
        Path(args.output).resolve() if args.output else input_path.parent / "l2_swimlane_trace.json"
    )
    func_names_path = Path(args.func_names).resolve() if args.func_names else None

    try:
        rc = convert_to_perfetto(
            input_path, output_path, func_names_path, args.all_views, args.verbose,
            args.case, args.spmd_tier
        )
    except Exception as exc:
        print(f"Error: conversion failed: {exc}", file=sys.stderr)
        return 1
    if rc != 0:
        return rc

    if not args.no_summary:
        print_perf_summary(input_path, args.case)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
