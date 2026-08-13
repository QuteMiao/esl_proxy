#!/usr/bin/env python3
"""
swimlane_records.json -> Perfetto (Chrome Trace Event Format)

用法:
    python3 tools/swimlane_to_perfetto.py [records.json] [-o out.json] [--summary-only]

不给输入路径时按以下顺序自动找:
    ./report/swimlane_records.json
    ./esl_proxy/report/swimlane_records.json

产物默认写在输入文件旁边, 名为 merged_swimlane.json。
打开 https://ui.perfetto.dev/ 把它拖进去即可。

trace 里有六个 process:
    Orchestrator   编排整段包络
    Cutter         每轮解依赖 / 提交边 (只记真干了活的轮次)
    Dispatcher     每轮收完成 / 派发 / ED 占槽
    Core           每个 (type, core) 一条泳道, 只画 run 段 —— 核占用总览
    Slot           每个 (type, core, slot) 一条泳道, 画 gated/wait/run/fin 四段
    Marks          ready / 门铃通知等瞬时事件
    另有 counter 轨: 同时在跑的核数、同时被 ED 占住的槽位数

为什么 Core 和 Slot 要分两层: 一个核有 AIC_OSTD 个槽位, ED 把任务 stage 到空闲
槽位上等门铃时, 该核另一个槽位可能正在跑别的任务 —— 两段时间是重叠的, 按核画会
撞在一起 (Perfetto 的同步事件要求同轨道严格嵌套)。按槽位画则天然不重叠。
Core 层只画 run 段, 而 executor 每个核同时只跑一个槽位, 所以也不会重叠。
"""

import argparse
import json
import os
import sys
from collections import defaultdict

# process id -> 显示名; 顺序即 Perfetto 里从上到下的排列
PID_ORCH = 1
PID_CUTTER = 2
PID_DISPATCH = 3
PID_CORE = 4
PID_SLOT = 5
PID_MARK = 6

PID_NAMES = {
    PID_ORCH: "1 Orchestrator",
    PID_CUTTER: "2 Cutter",
    PID_DISPATCH: "3 Dispatcher",
    PID_CORE: "4 Core (run only)",
    PID_SLOT: "5 Slot detail",
    PID_MARK: "6 Task marks",
}

# 段名 -> Perfetto 内置配色。cq_build_running 等是 UI 认识的固定色板名。
SEGMENT_COLORS = {
    "gated": "thread_state_unknown",       # 灰: 占着槽位但不能跑
    "wait": "thread_state_runnable",       # 浅: 可跑但核还没轮到
    "run": "thread_state_running",         # 实色: 真正执行
    "fin": "thread_state_iowait",          # 完成到被 dispatcher 看见
}

DEFAULT_INPUTS = [
    "report/swimlane_records.json",
    "esl_proxy/report/swimlane_records.json",
]


def us(ns):
    """ns -> µs; Chrome Trace 的时间单位是微秒。"""
    return ns / 1000.0


def core_lane_name(type_id, core):
    return f"{'CUBE' if type_id == 0 else 'VEC'}_{core:02d}"


def find_input():
    for cand in DEFAULT_INPUTS:
        if os.path.isfile(cand):
            return cand
    return None


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def add_process_metadata(events, pid, sort_index):
    events.append({"ph": "M", "name": "process_name", "pid": pid, "tid": 0,
                   "args": {"name": PID_NAMES[pid]}})
    events.append({"ph": "M", "name": "process_sort_index", "pid": pid, "tid": 0,
                   "args": {"sort_index": sort_index}})


def add_thread_metadata(events, pid, tid, name, sort_index):
    events.append({"ph": "M", "name": "thread_name", "pid": pid, "tid": tid,
                   "args": {"name": name}})
    events.append({"ph": "M", "name": "thread_sort_index", "pid": pid, "tid": tid,
                   "args": {"sort_index": sort_index}})


def slice_event(pid, tid, name, start_ns, end_ns, args=None, color=None):
    """
    一条完整的 X (complete) 事件。

    宽度为 0 的段照实输出, 不做下限钳位。EXEC_DURATION_SCALE 默认 10000, 多数任务
    缩放后只跑 1 tick, run 段真的会退化成 0 宽; 而任何正数下限都会把该段的尾巴推过
    紧随其后的 fin 段起点, 变成 Perfetto 不接受的部分重叠。想看清细节应该调小
    EXEC_DURATION_SCALE 重跑, 而不是在这里造宽度。
    """
    dur = max(end_ns - start_ns, 0)
    ev = {
        "ph": "X",
        "pid": pid,
        "tid": tid,
        "name": name,
        "ts": us(start_ns),
        "dur": us(dur),
    }
    if args:
        ev["args"] = args
    if color:
        ev["cname"] = color
    return ev


def build_counter_events(pid, name, deltas):
    """
    把 (时刻, 增量) 序列累加成 Perfetto counter 轨。
    同一时刻的多个增量必须先合并再输出, 否则 counter 会出现锯齿假象。
    """
    merged = defaultdict(int)
    for ts_ns, delta in deltas:
        merged[ts_ns] += delta

    events = []
    running = 0
    for ts_ns in sorted(merged):
        running += merged[ts_ns]
        events.append({
            "ph": "C",
            "pid": pid,
            "tid": 0,
            "name": name,
            "ts": us(ts_ns),
            "args": {name: running},
        })
    return events


def convert(data):
    tasks = data["tasks"]
    phases = data["phases"]
    marks = data["marks"]
    phase_names = data["phase_names"]
    mark_names = data["mark_names"]
    path_names = data["path_names"]
    meta = data["meta"]

    events = []
    for idx, pid in enumerate(sorted(PID_NAMES)):
        add_process_metadata(events, pid, idx)

    # ── 控制面: orch / cutter / dispatcher ────────────────────────────────
    role_to_pid = {0: PID_ORCH, 1: PID_CUTTER, 2: PID_DISPATCH}
    seen_control_lanes = set()
    for role, kind, tid, sub, n, start_ns, end_ns in phases:
        pid = role_to_pid[role]
        if (pid, tid) not in seen_control_lanes:
            seen_control_lanes.add((pid, tid))
            lane = "orch" if pid == PID_ORCH else f"thread_{tid}"
            add_thread_metadata(events, pid, tid, lane, tid)

        name = phase_names[kind]
        args = {"tasks": n}
        # disp_send 的 sub 是 task type, 分开显示才能看出 CUBE/VECTOR 各自的派发节奏
        if phase_names[kind] == "disp_send":
            name = f"send_{['cube', 'vector', 'mix'][sub] if sub < 3 else sub}"
            args["task_type"] = sub
        events.append(slice_event(pid, tid, name, start_ns, end_ns, args))

    # ── Worker: Core 层 (只画 run) 与 Slot 层 (四段全画) ──────────────────
    seen_core_lanes = set()
    seen_slot_lanes = set()
    run_deltas = []
    gated_deltas = []

    for rec in tasks:
        (task_id, core, type_id, slot, path, blocks,
         stage_ns, pub_ns, run_ns, end_ns, finish_ns) = rec

        lane = core_lane_name(type_id, core)
        # tid 必须全局唯一且稳定; type/core/slot 编码进去即可
        core_tid = type_id * 1000 + core
        slot_tid = type_id * 10000 + core * 10 + slot

        if core_tid not in seen_core_lanes:
            seen_core_lanes.add(core_tid)
            add_thread_metadata(events, PID_CORE, core_tid, lane, core_tid)
        if slot_tid not in seen_slot_lanes:
            seen_slot_lanes.add(slot_tid)
            add_thread_metadata(events, PID_SLOT, slot_tid, f"{lane}/s{slot}", slot_tid)

        label = f"T{task_id}"
        if blocks > 1:
            label += f" x{blocks}"
        args = {
            "task_id": task_id,
            "path": path_names[path],
            "core": core,
            "slot": slot,
            "blocks": blocks,
            "gated_us": round(us(pub_ns - stage_ns), 3) if stage_ns else 0.0,
            "wait_us": round(us(run_ns - pub_ns), 3),
            "run_us": round(us(end_ns - run_ns), 3),
            "fin_us": round(us(finish_ns - end_ns), 3) if finish_ns else 0.0,
        }

        events.append(slice_event(PID_CORE, core_tid, label, run_ns, end_ns, args,
                                  SEGMENT_COLORS["run"]))
        run_deltas.append((run_ns, 1))
        run_deltas.append((end_ns, -1))

        if stage_ns:
            events.append(slice_event(PID_SLOT, slot_tid, f"gated {label}",
                                      stage_ns, pub_ns, args, SEGMENT_COLORS["gated"]))
            gated_deltas.append((stage_ns, 1))
            gated_deltas.append((pub_ns, -1))
        if run_ns > pub_ns:
            events.append(slice_event(PID_SLOT, slot_tid, f"wait {label}",
                                      pub_ns, run_ns, args, SEGMENT_COLORS["wait"]))
        events.append(slice_event(PID_SLOT, slot_tid, label, run_ns, end_ns, args,
                                  SEGMENT_COLORS["run"]))
        if finish_ns > end_ns:
            events.append(slice_event(PID_SLOT, slot_tid, f"fin {label}",
                                      end_ns, finish_ns, args, SEGMENT_COLORS["fin"]))

    # ── 瞬时事件: 每种 mark 一条泳道 ──────────────────────────────────────
    seen_mark_lanes = set()
    for task_id, kind, ns in marks:
        if kind not in seen_mark_lanes:
            seen_mark_lanes.add(kind)
            add_thread_metadata(events, PID_MARK, kind, mark_names[kind], kind)
        events.append({
            "ph": "i", "s": "t",
            "pid": PID_MARK, "tid": kind,
            "name": f"{mark_names[kind]} T{task_id}",
            "ts": us(ns),
            "args": {"task_id": task_id},
        })

    events.extend(build_counter_events(PID_CORE, "cores_running", run_deltas))
    if gated_deltas:
        events.extend(build_counter_events(PID_SLOT, "slots_gated", gated_deltas))

    return {
        "traceEvents": events,
        "displayTimeUnit": "ns",
        "otherData": {k: str(v) for k, v in meta.items()},
    }


def print_summary(data):
    tasks = data["tasks"]
    meta = data["meta"]
    if not tasks:
        print("no task records")
        return

    span_ns = max(r[9] for r in tasks) - min(r[8] for r in tasks)
    ed = [r for r in tasks if r[4] == 1]
    busy_ns = defaultdict(int)
    for r in tasks:
        busy_ns[(r[2], r[1])] += r[9] - r[8]

    def mean(xs):
        return sum(xs) / len(xs) if xs else 0.0

    print(f"case                 {meta['case']}  (ED={meta['ed_enable']}, "
          f"ostd={meta['aic_ostd']}, scale={meta['exec_duration_scale']})")
    print(f"tasks                {len(tasks)}  (ED path {len(ed)}, "
          f"{100.0 * len(ed) / len(tasks):.1f}%)")
    print(f"span                 {us(span_ns):.1f} us")
    print(f"cores used           {len(busy_ns)} / {meta['aic_cnt'] * meta['exe_type_cnt']}")
    print(f"core busy mean       {100.0 * mean(list(busy_ns.values())) / span_ns:.1f}% "
          f"of span")
    print(f"run mean             {us(mean([r[9] - r[8] for r in tasks])):.2f} us")
    print(f"pub->run wait mean   {us(mean([r[8] - r[7] for r in tasks])):.2f} us  "
          f"(等核空闲, 即预装载位排队)")
    print(f"end->finish mean     {us(mean([r[10] - r[9] for r in tasks if r[10]])):.2f} us  "
          f"(dispatcher 看见完成的延迟)")
    if ed:
        print(f"ED gated mean        {us(mean([r[7] - r[6] for r in ed])):.2f} us  "
              f"(占着槽位等门铃)")

    dropped = data.get("dropped", {})
    if any(dropped.values()):
        print(f"WARNING dropped      {dropped} — 调大 swimlane.h 里的 SWIM_MAX_*")


def main():
    parser = argparse.ArgumentParser(
        description="swimlane_records.json -> Perfetto trace")
    parser.add_argument("input", nargs="?", help="swimlane_records.json 路径")
    parser.add_argument("-o", "--output", help="输出 trace 路径")
    parser.add_argument("--summary-only", action="store_true",
                        help="只打印统计, 不生成 trace")
    args = parser.parse_args()

    path = args.input or find_input()
    if path is None:
        print("找不到 swimlane_records.json。先用 make SWIMLANE=1 ... run 跑一次，"
              "或显式给出路径。", file=sys.stderr)
        return 1
    if not os.path.isfile(path):
        print(f"输入不存在: {path}", file=sys.stderr)
        return 1

    data = load(path)
    print_summary(data)

    if args.summary_only:
        return 0

    out = args.output or os.path.join(os.path.dirname(path) or ".",
                                      "merged_swimlane.json")
    trace = convert(data)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(trace, f, separators=(",", ":"))
    print(f"\ntrace written        {out}  ({len(trace['traceEvents'])} events)")
    print("打开 https://ui.perfetto.dev/ 拖进去即可")
    return 0


if __name__ == "__main__":
    sys.exit(main())
