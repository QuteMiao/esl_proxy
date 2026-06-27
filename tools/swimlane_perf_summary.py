#!/usr/bin/env python3
"""Summarize esl_proxy l2_swimlane_records.json for quick perf comparison."""
from __future__ import annotations

import json
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: swimlane_perf_summary.py <records.json> <case_name>", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    case = sys.argv[2]
    data = json.loads(path.read_text())
    freq = int(data.get("metadata", {}).get("clock_freq_hz", 50_000_000))
    tasks = data.get("aicore_tasks", [])
    durs = []
    starts = []
    ends = []
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

    if not durs:
        return 1

    span = max(ends) - min(starts)
    total = sum(durs)
    n = len(durs)
    print(
        f"swimlane: case={case} tasks={n} cores={data.get('metadata', {}).get('num_cores')} "
        f"span_ms={span / freq * 1000:.2f} avg_task_us={total / n / freq * 1e6:.1f} "
        f"ktasks_per_s={n / (span / freq) / 1000:.2f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
