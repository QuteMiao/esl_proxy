#!/usr/bin/env python3
"""
Analyze scheduler log output from esl_proxy and generate:
  1. DAG dependency graph (DOT + SVG via Graphviz)
  2. Swimlane chart showing task execution across cores (SVG via matplotlib)

Usage:
    python3 tools/analyze_scheduler_logs.py [--log-dir <dir>] [--output-dir <dir>]

Input:  log/scheduler_thread_*.csv  (default: log/)
Output: report/dag.svg, report/swimlane.svg (default: report/)
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

CSV_HEADER_COLS = ["file", "line", "detail"]


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
            # next token looks like a value (numeric or hex)
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
        header = next(reader, None)  # skip header
        for idx, row in enumerate(reader):
            if len(row) < 3:
                continue
            detail = ",".join(row[2:])
            parsed = parse_key_value(detail)
            parsed["_file"] = row[0]
            parsed["_line"] = int(row[1])
            parsed["_row"] = idx  # 0-based row index in data lines
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
    """From all painter threads, extract edges (pred → succ) and task types.

    Returns:
        edges: list of (predecessor_id, successor_id)
        task_types: dict task_id -> int (0=CUBE, 1=VECTOR)
    """
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

    # Deduplicate while preserving order
    seen = set()
    unique_edges = []
    for e in edges:
        if e not in seen:
            seen.add(e)
            unique_edges.append(e)

    return unique_edges, task_types


# ── Execution timeline extraction ────────────────────────────────────────────
def extract_timeline(dispatch_rows_list: list[list[dict]]) -> list[dict]:
    """From all dispatch threads, extract send/completed events as timeline.

    Returns list of dicts:
        { "event": "send"|"complete", "task_id": int, "core": int,
          "type": int, "thread": int, "row": int (global row index) }
    Each event also carries a monotonic 'seq' for ordering.
    """
    events = []
    for thread_idx, rows in enumerate(dispatch_rows_list):
        for row in rows:
            if "send" in row and "task_id" in row and "core" in row:
                events.append({
                    "event": "send",
                    "task_id": int(row["task_id"]),
                    "core": int(row["core"]),
                    "type": int(row.get("type", 0)) if row.get("type") is not None else 0,
                    "thread": thread_idx,
                    "row": row["_row"],
                })
            elif "completed" in row and "task_id" in row and "core" in row:
                events.append({
                    "event": "complete",
                    "task_id": int(row["task_id"]),
                    "core": int(row["core"]),
                    "type": 0,
                    "thread": thread_idx,
                    "row": row["_row"],
                })

    # Sort by global ordering heuristic: interleave thread rows by timestamp proxy
    # Since we don't have real timestamps, we sort by (row_within_thread, thread)
    events.sort(key=lambda e: (e["row"], e["thread"]))
    for i, e in enumerate(events):
        e["seq"] = i
    return events


def build_swimlane_intervals(events: list[dict]) -> list[dict]:
    """Pair send→complete events to form execution intervals per core.

    Returns list of dicts:
        { "task_id", "core", "type", "start_seq", "end_seq" }
    """
    # Pending sends: (task_id, core) -> event
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
    # Unmatched sends (no complete seen) — give them a short dummy interval
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


# ── Swimlane output ──────────────────────────────────────────────────────────
def generate_swimlane_svg(intervals: list[dict], task_types: dict,
                          output_path: str):
    """Generate a standalone SVG swimlane chart using only stdlib (no matplotlib)."""
    if not intervals:
        print("[Swimlane] No intervals to plot.")
        return

    cores_set = sorted({iv["core"] for iv in intervals})
    max_seq = max(iv["end_seq"] for iv in intervals) if intervals else 1
    num_cores = len(cores_set)
    core_to_row = {c: i for i, c in enumerate(cores_set)}

    # Layout constants
    margin_left = 120
    margin_top = 60
    margin_right = 40
    margin_bottom = 40
    lane_height = 80
    bar_height = 44
    header_height = 30
    px_per_seq = 40  # Scale: pixels per sequence unit
    time_width = max(800, max_seq * px_per_seq)
    total_width = margin_left + time_width + margin_right
    total_height = margin_top + header_height + num_cores * lane_height + margin_bottom

    svg_parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="0 0 {total_width} {total_height}" '
        f'width="100%" height="100%">',
        '<style>',
        '  text { font-family: Arial, sans-serif; font-size: 14px; }',
        '  .core-label { font-weight: bold; font-size: 18px; }',
        '  .task-label { font-size: 13px; fill: #fff; font-weight: bold; }',
        '  .task-label-dark { font-size: 13px; fill: #000; font-weight: bold; }',
        '  .axis-label { font-size: 12px; fill: #555; }',
        '  .grid-line { stroke: #e0e0e0; stroke-width: 0.5; }',
        '</style>',
        f'<rect width="{total_width}" height="{total_height}" fill="#ffffff"/>',
    ]

    # Title
    svg_parts.append(
        f'<text x="{total_width/2}" y="30" text-anchor="middle" '
        f'font-size="16" font-weight="bold" fill="#333">'
        f'Task Execution Swimlane</text>'
    )

    # Lane backgrounds
    for ci, core in enumerate(cores_set):
        y = margin_top + header_height + ci * lane_height
        color = "#f5f5f5" if ci % 2 == 0 else "#ffffff"
        svg_parts.append(
            f'<rect x="{margin_left}" y="{y}" width="{time_width}" '
            f'height="{lane_height}" fill="{color}" stroke="#ccc" '
            f'stroke-width="0.5"/>'
        )
        svg_parts.append(
            f'<text x="{margin_left - 10}" y="{y + lane_height/2 + 4}" '
            f'text-anchor="end" class="core-label" fill="#333">'
            f'Core {core}</text>'
        )

    # Task bars
    colors_palette = [
        "#4caf50", "#2196f3", "#ff9800", "#9c27b0",
        "#00bcd4", "#e91e63", "#3f51b5", "#ff5722",
        "#009688", "#795548", "#607d8b", "#f44336",
        "#8bc34a", "#03a9f4", "#ffeb3b",
    ]
    for iv in intervals:
        core_row = core_to_row[iv["core"]]
        y_center = margin_top + header_height + core_row * lane_height + lane_height / 2
        y_bar = y_center - bar_height / 2
        x_start = margin_left + iv["start_seq"] * px_per_seq
        duration = max(iv["end_seq"] - iv["start_seq"], 1)
        width = duration * px_per_seq
        color = colors_palette[iv["task_id"] % len(colors_palette)]
        ttype = task_types.get(iv["task_id"], None)
        type_str = TASK_TYPE_NAMES.get(ttype, "") if ttype is not None else ""

        svg_parts.append(
            f'<rect x="{x_start}" y="{y_bar}" width="{width}" '
            f'height="{bar_height}" rx="3" ry="3" fill="{color}" '
            f'stroke="#fff" stroke-width="1" opacity="0.85"/>'
        )
        # Label
        if width > 30:
            label = f'T{iv["task_id"]} {type_str}'
        else:
            label = f'T{iv["task_id"]}'
        # Use white text on darker bars, black on light bars
        is_light = color in ("#ffeb3b", "#8bc34a", "#03a9f4", "#4caf50")
        label_class = "task-label-dark" if is_light else "task-label"
        svg_parts.append(
            f'<text x="{x_start + width/2}" y="{y_center + 4}" '
            f'text-anchor="middle" class="{label_class}">'
            f'{label}</text>'
        )

    # Legend
    legend_x = margin_left
    legend_y = total_height - 18
    svg_parts.append(
        f'<text x="{legend_x}" y="{legend_y}" font-size="10" fill="#888">'
        f'X-axis: event sequence number (no real timestamps available)</text>'
    )

    svg_parts.append("</svg>")

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w") as f:
        f.write("\n".join(svg_parts))
    print(f"[Swimlane] SVG written → {output_path}")


# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Analyze scheduler logs → DAG + Swimlane"
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

    # Discover CSV log files
    csv_files = sorted(log_dir.glob("scheduler_thread_*.csv"))
    if not csv_files:
        print(f"Error: no scheduler_thread_*.csv files found in {log_dir}")
        sys.exit(1)

    print(f"Found {len(csv_files)} log file(s) in {log_dir}")

    # Classify threads
    painter_rows_list = []
    dispatch_rows_list = []
    for fp in csv_files:
        role = determine_thread_role(str(fp))
        rows = read_csv_log(str(fp))
        print(f"  {fp.name}: {role} ({len(rows)} rows)")
        if role == "painter":
            painter_rows_list.append(rows)
        elif role == "dispatch":
            dispatch_rows_list.append(rows)

    # Extract DAG
    edges, task_types = extract_dag(painter_rows_list)
    print(f"\nDAG: {len(task_types)} tasks, {len(edges)} dependency edges")

    # Extract timeline
    events = extract_timeline(dispatch_rows_list)
    send_count = sum(1 for e in events if e["event"] == "send")
    complete_count = sum(1 for e in events if e["event"] == "complete")
    print(f"Timeline: {len(events)} events ({send_count} send, {complete_count} complete)")

    intervals = build_swimlane_intervals(events)
    print(f"Swimlane: {len(intervals)} execution intervals")

    # Output DAG
    out_dir = Path(args.output_dir)
    dot_path = out_dir / "dag.dot"
    svg_path = out_dir / "dag.svg"
    generate_dag_dot(edges, task_types, str(dot_path))
    render_dot(str(dot_path), str(svg_path))

    # Output Swimlane
    swimlane_path = out_dir / "swimlane.svg"
    generate_swimlane_svg(intervals, task_types, str(swimlane_path))

    print(f"\nDone. Output in {out_dir.resolve()}/")


if __name__ == "__main__":
    main()