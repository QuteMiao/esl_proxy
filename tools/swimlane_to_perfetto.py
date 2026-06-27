#!/usr/bin/env python3
"""Convert esl_proxy l2_swimlane_records.json to Chrome Trace (Perfetto) JSON.

Raw swimlane dumps are an intermediate schema for analysis tools; Perfetto
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


def _resolve_func_ids(tasks: list, func_names: dict[str, str]) -> None:
    """Fill func_id when host export leaves it at -1 (onboard has no deps.json)."""
    if not func_names:
        return
    mod_n = max(int(k) for k in func_names) + 1 if func_names else 0
    if mod_n <= 0:
        return
    for task in tasks:
        if int(task.get("func_id", -1)) >= 0:
            continue
        tid = int(task.get("task_id", 0))
        fid = tid % mod_n
        task["func_id"] = fid


def _filter_aicore_view_only(events: list) -> list:
    """Keep only Worker View (pid=4): AICore kernel start→end slices and lane metadata."""
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert l2_swimlane_records.json to Perfetto-compatible Chrome trace JSON"
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
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    esl_root = Path(__file__).resolve().parents[1]
    input_path = Path(args.input).resolve()
    if not input_path.is_file():
        print(f"Error: input not found: {input_path}", file=sys.stderr)
        return 1

    output_path = Path(args.output).resolve() if args.output else input_path.parent / "l2_swimlane_trace.json"
    func_names_path = Path(args.func_names).resolve() if args.func_names else None
    func_names = _load_func_names(func_names_path, esl_root)

    try:
        conv = _load_swimlane_converter()
        data = conv.read_perf_data(input_path)
        _resolve_func_ids(data["tasks"], func_names)
        conv.generate_chrome_trace_json(
            data["tasks"],
            str(output_path),
            func_names,
            args.verbose,
            scheduler_phases=data.get("aicpu_scheduler_phases"),
            orchestrator_phases=data.get("aicpu_orchestrator_phases"),
            core_to_thread=data.get("core_to_thread"),
            deps_edges=None,
            deps_kernel_map=None,
            emit_overhead=False,
        )
    except Exception as exc:
        print(f"Error: conversion failed: {exc}", file=sys.stderr)
        return 1

    with open(output_path) as f:
        trace = json.load(f)
    if "traceEvents" not in trace:
        print("Error: output missing traceEvents wrapper", file=sys.stderr)
        return 1

    if not args.all_views:
        trace["traceEvents"] = _filter_aicore_view_only(trace["traceEvents"])

    _apply_kernel_colors(trace["traceEvents"])

    with open(output_path, "w") as f:
        json.dump(trace, f, indent=2)

    n = len(trace["traceEvents"])
    view_note = "AICore View only" if not args.all_views else "all views"
    names_note = f", {len(func_names)} kernel names" if func_names else ""
    print(f"Perfetto trace written: {output_path} ({n} events, {view_note}{names_note})")
    print("Open https://ui.perfetto.dev/ and drag the file to visualize.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
