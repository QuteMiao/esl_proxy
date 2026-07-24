#!/usr/bin/env python3
"""
Analyze scheduler log output from esl_proxy and generate:
  1. DAG dependency graph (DOT + SVG via Graphviz)
  2. Per-dispatch-thread swimlane charts — one SVG per dispatch CSV file

Usage:
    python3 tools/analyze_scheduler_logs.py [--log-dir <dir>] [--output-dir <dir>]

Input:  log/scheduler_thread_*.csv  (default: log/)
Output: report/dag.svg, report/swimlane_thread_<N>.svg (default: report/)
"""

import argparse
import csv
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

# ── Constants ───────────────────────────────────────────────────────────────
TASK_TYPE_NAMES = {0: "CUBE", 1: "VECTOR"}
TASK_TYPE_COLORS = {0: "#ff9800", 1: "#9c27b0"}
TASK_TYPE_SHAPES = {0: "rect", 1: "ellipse"}


# ── Parsing helpers ──────────────────────────────────────────────────────────
def parse_key_value(detail: str) -> dict:
    """Parse comma-separated key,value pairs from a log detail string.

    e.g. "ready,task_id,0,pre_cnt,0,type,0,cnt,1" → {"ready": None, "task_id": "0", ...}
    Keys with no following value get a None marker.
    """
    parts = [p.strip() for p in detail.split(",")]
    result = {}
    i = 0
    while i < len(parts):
        key = parts[i]
        if i + 1 < len(parts) and not parts[i + 1].replace("_", "").isalpha():
            result[key] = parts[i + 1]
            i += 2
        else:
            result[key] = None
            i += 1
    return result


def read_csv_log(filepath: str) -> list[dict]:
    """Read a scheduler_thread_*.csv and return list of parsed rows."""
    rows = []
    if not os.path.exists(filepath):
        return rows
    with open(filepath, "r", encoding="utf-8") as f:
        reader = csv.reader(f)
        next(reader, None)  # skip header
        for idx, row in enumerate(reader):
            if len(row) < 3:
                continue
            detail = ",".join(row[2:])
            parsed = parse_key_value(detail)
            parsed["_file"] = row[0]
            parsed["_line"] = int(row[1])
            parsed["_row"] = idx
            rows.append(parsed)
    return rows


def determine_thread_role(filepath: str) -> str:
    """Heuristic: inspect first few rows to classify the thread."""
    rows = read_csv_log(filepath)
    for row in rows[:5]:
        detail_keys = list(row.keys())
        if "dispatch" in detail_keys:
            return "dispatch"
        if "painter" in detail_keys:
            return "painter"
        if "painter_cnt" in detail_keys:
            return "main"
    return "unknown"


# ── DAG extraction ───────────────────────────────────────────────────────────
def extract_dag(painter_rows_list: list[list[dict]]) -> tuple[list, dict]:
    """From all painter threads, extract edges (pred → succ) and task types."""
    edges = []
    task_types = {}
    for rows in painter_rows_list:
        for row in rows:
            if "ready" in row and "task_id" in row:
                tid = int(row["task_id"])
                typ = int(row["type"])
                task_types[tid] = typ
            if "add" in row and "task_id" in row and "successor_id" in row:
                pred_id = int(row["task_id"])
                succ_id = int(row["successor_id"])
                edges.append((pred_id, succ_id))
    seen = set()
    unique_edges = []
    for e in edges:
        if e not in seen:
            seen.add(e)
            unique_edges.append(e)
    return unique_edges, task_types


# ── Per-file timeline extraction ─────────────────────────────────────────────
def extract_timeline_from_file(rows: list[dict]) -> list[dict]:
    """From a single dispatch CSV file, extract send/completed events in order.

    Returns list of dicts:
        { "event": "send"|"complete", "task_id": int, "core": int,
          "type": int, "seq": int (row order within file) }
    """
    events = []
    for row in rows:
        if "send" in row and "task_id" in row and "core" in row:
            events.append({
                "event": "send",
                "task_id": int(row["task_id"]),
                "core": int(row["core"]),
                "type": int(row.get("type", 0)) if row.get("type") is not None else 0,
                "seq": len(events),
            })
        elif "completed" in row and "task_id" in row and "core" in row:
            events.append({
                "event": "complete",
                "task_id": int(row["task_id"]),
                "core": int(row["core"]),
                "type": 0,
                "seq": len(events),
            })
    return events


def build_swimlane_intervals(events: list[dict]) -> list[dict]:
    """Pair send→complete events to form execution intervals per core.

    Returns list of dicts:
        { "task_id", "core", "type", "start_seq", "end_seq" }
    """
    pending = {}
    intervals = []
    for ev in events:
        key = (ev["task_id"], ev["core"])
        if ev["event"] == "send":
            pending[key] = ev
        elif ev["event"] == "complete":
            if key in pending:
                send_ev = pending.pop(key)
                intervals.append({
                    "task_id": ev["task_id"],
                    "core": ev["core"],
                    "type": send_ev["type"],
                    "start_seq": send_ev["seq"],
                    "end_seq": ev["seq"],
                })
    for key, send_ev in pending.items():
        intervals.append({
            "task_id": send_ev["task_id"],
            "core": send_ev["core"],
            "type": send_ev["type"],
            "start_seq": send_ev["seq"],
            "end_seq": send_ev["seq"] + 1,
        })
    return intervals


# ── DAG output ───────────────────────────────────────────────────────────────
def generate_dag_dot(edges: list, task_types: dict, output_path: str):
    """Write Graphviz DOT file for the DAG."""
    nodes = set()
    for p, s in edges:
        nodes.add(p)
        nodes.add(s)
    for tid in task_types:
        nodes.add(tid)

    lines = [
        "digraph DAG {",
        '  rankdir=TB;',
        '  bgcolor="#ffffff";',
        '  node [fontname="Arial" fontcolor="#000000"];',
        '  edge [color="#666666" fontname="Arial"];',
        "",
    ]
    for nid in sorted(nodes):
        ttype = task_types.get(nid, 2)
        tname = TASK_TYPE_NAMES.get(ttype, "UNKNOWN")
        shape = TASK_TYPE_SHAPES.get(ttype, "rect")
        color = TASK_TYPE_COLORS.get(ttype, "#cccccc")
        lines.append(
            f'  T{nid} [label="T{nid}\\n{tname}" shape={shape} '
            f'fillcolor="{color}" style=filled fontcolor="#000000"];'
        )
    lines.append("")
    for pred, succ in sorted(edges):
        lines.append(f"  T{pred} -> T{succ};")
    lines.append("}")

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w") as f:
        f.write("\n".join(lines))
    print(f"[DAG] DOT written → {output_path}")


def render_dot(dot_path: str, output_path: str, fmt: str = "svg") -> bool:
    """Render DOT file to image using Graphviz dot command."""
    try:
        result = subprocess.run(
            ["dot", "-T", fmt, "-o", output_path, dot_path],
            capture_output=True, text=True,
        )
        if result.returncode == 0:
            print(f"[DAG] Rendered → {output_path}")
            return True
        else:
            print(f"[DAG] dot error: {result.stderr.strip()}")
            return False
    except FileNotFoundError:
        print("[DAG] Warning: 'dot' not found. Install graphviz: brew install graphviz")
        return False


# ── Swimlane output (one per dispatch file) ──────────────────────────────────
def generate_swimlane_csv(intervals: list[dict], output_path: str):
    """Generate a CSV swimlane: first column = core, rest = task_id sequence."""
    if not intervals:
        print(f"[Swimlane] No intervals to output → {output_path} (skipped)")
        return

    core_sequences = defaultdict(list)
    for iv in intervals:
        core_sequences[iv["core"]].append(iv)

    for core in core_sequences:
        core_sequences[core].sort(key=lambda iv: iv["start_seq"])

    cores_sorted = sorted(core_sequences.keys())
    max_len = max(len(core_sequences[c]) for c in cores_sorted) if cores_sorted else 0

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w", newline="") as f:
        writer = csv.writer(f)
        header = ["core"] + [f"seq_{i}" for i in range(1, max_len + 1)]
        writer.writerow(header)
        for core in cores_sorted:
            row = [str(core)] + [str(iv["task_id"]) for iv in core_sequences[core]]
            writer.writerow(row)

    print(f"[Swimlane] CSV written → {output_path} "
          f"({len(cores_sorted)} cores, max {max_len} tasks per core)")


# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Analyze scheduler logs → DAG + per-thread Swimlanes"
    )
    parser.add_argument(
        "--log-dir", default="log",
        help="Directory containing scheduler_thread_*.csv files (default: log/)"
    )
    parser.add_argument(
        "--output-dir", default="report",
        help="Directory for output images (default: report/)"
    )
    args = parser.parse_args()

    log_dir = Path(args.log_dir)
    if not log_dir.is_dir():
        print(f"Error: log directory not found: {log_dir}")
        sys.exit(1)

    csv_files = sorted(log_dir.glob("scheduler_thread_*.csv"))
    if not csv_files:
        print(f"Error: no scheduler_thread_*.csv files found in {log_dir}")
        sys.exit(1)

    print(f"Found {len(csv_files)} log file(s) in {log_dir}")

    # Classify threads
    painter_rows_list = []
    dispatch_files = []  # (filename_stem, rows)
    for fp in csv_files:
        role = determine_thread_role(str(fp))
        rows = read_csv_log(str(fp))
        print(f"  {fp.name}: {role} ({len(rows)} rows)")
        if role == "painter":
            painter_rows_list.append(rows)
        elif role == "dispatch":
            dispatch_files.append((fp.stem, rows))

    # Extract DAG from painter threads
    edges, task_types = extract_dag(painter_rows_list)
    print(f"\nDAG: {len(task_types)} tasks, {len(edges)} dependency edges")

    out_dir = Path(args.output_dir)

    # Output DAG
    dot_path = out_dir / "dag.dot"
    dag_svg_path = out_dir / "dag.svg"
    generate_dag_dot(edges, task_types, str(dot_path))
    render_dot(str(dot_path), str(dag_svg_path))

    # Output one swimlane per dispatch file
    for filename_stem, rows in dispatch_files:
        events = extract_timeline_from_file(rows)
        send_count = sum(1 for e in events if e["event"] == "send")
        complete_count = sum(1 for e in events if e["event"] == "complete")
        print(f"\n  [{filename_stem}] {len(events)} events ({send_count} send, {complete_count} complete)")

        intervals = build_swimlane_intervals(events)
        print(f"  [{filename_stem}] {len(intervals)} execution intervals")

        swimlane_path = out_dir / f"swimlane_{filename_stem}.csv"
        generate_swimlane_csv(intervals, str(swimlane_path))

    print(f"\nDone. Output in {out_dir.resolve()}/")


if __name__ == "__main__":
    main()