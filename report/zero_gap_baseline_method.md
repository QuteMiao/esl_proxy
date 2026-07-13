# 零调度开销泳道图（Zero-Gap Baseline）构建方法

## 1. 目的

在实际调度场景中，任务之间的时间间隔（gap）包含调度开销（dispatch delay）、
依赖等待（dependency wait）等非计算时间。为了量化这些开销的大小，需要构建一个
**消除所有调度时间的理论基线泳道图**，作为对比参照。

零调度开销泳道图回答的问题：**如果调度延迟为零——前序任务完成后后序任务立刻开始，
整体执行能快多少？**

## 2. 构建原理

### 核心思想

从代码出发分析任务 DAG 依赖关系，在保证 **DAG 约束**（前序任务完成后后序任务才能
开始）的前提下，将前序任务与后序任务之间的调度间隔置为 0：

- **保持每个子任务所在的核不变**：每个子任务仍在原始分配的核上执行。
- **保持每个核上的子任务执行顺序不变**：按原始 start 时间排序。
- **DAG 依赖约束**：一个任务只有在其所有前驱任务都完成后才能开始。
- **零调度间隔**：子任务启动时间 = max(前驱任务完成时间, 该核空闲时间)，
  无额外调度延迟。

### DAG 依赖提取

DAG 依赖关系通过运行 esl_proxy 仿真提取：

1. 使用 `WORKER_LOG=1` 编译仿真程序
2. 运行仿真，生成 `log/pto._thread_*.csv` 日志文件
3. 从日志中解析 `succeed,task_id,X,predecessor_id,Y` 条目，构建前驱关系

对于 qwen3 系列案例，每个 SPMD tier 对应不同的任务粒度，需要分别提取 DAG。
`qwen3_dynamic_tensormap` 与 `qwen3_dynamic_manual_scope` 共享相同的 DAG
（任务结构相同，仅 tensor wiring 不同）。

### 子任务级调度模型

每个泳道行是一个 **子任务（subtask）**——在一个核上的独立执行单元。一个任务
可能有多个子任务分布在不同的核上（SPMD 场景）。

- 一个任务 **就绪（ready）** 当且仅当其所有 DAG 前驱任务都已 **完成**（所有子任务结束）。
- 每个子任务独立调度：`start = max(task_ready_time, core_free[core])`
- 同一任务的子任务可以在不同核上以不同时间启动（模拟实际 SPMD 分发行为）
- 一个任务 **完成** 当其最后一个子任务结束时

### 算法

```
输入：
  - 原始泳道记录 aicore_tasks = [[core_id, task_token, reg_task_id, start, end], ...]
  - DAG 前驱关系 preds = {task_id: [predecessor_task_ids]}

1. 加载泳道数据，按 task_id 分组子任务：
   task_subtasks[tid] = [(core, dur_ns, actual_start), ...]

2. 按 core 分组，构建每核的子任务队列（按 actual_start 排序）：
   core_queues[core] = [(task_id, dur_ns, actual_start), ...]

3. 迭代调度：
   - 找到所有"队首子任务"中，任务 DAG 已就绪（所有前驱已完成）且
     可最早启动的子任务
   - 调度该子任务：
     start = max(task_ready_time, core_free[core])
     end = start + dur_ns
   - 更新 core_free[core] = end
   - 更新 task_end[tid] = max(task_end[tid], end)
   - 当任务所有子任务调度完毕，标记 task_finish[tid] = task_end[tid]
   - 该核队首指针前移，继续下一轮
   - 重复直到所有子任务调度完毕

输出：基线泳道记录 [[core_id, task_token, reg_task_id, new_start, new_end], ...]
```

### 正确性保证

由于保持了每核子任务顺序且调度间隔为零，通过拓扑归纳可证明：

- 基线中每个子任务的启动时间 ≤ 实际启动时间
- 因此基线 makespan ≤ 实际 makespan

这保证了基线不会比实际更慢。

### 关键设计决策

| 决策点 | 选择 | 原因 |
|--------|------|------|
| DAG 来源 | 仿真日志（WORKER_LOG=1） | 直接从代码执行中提取真实依赖关系 |
| 核上子任务顺序 | 保持原始 actual_start 顺序 | 保持实际运行时的执行顺序 |
| 任务执行时长 | 保持原始 `end - start` 不变 | 只消除调度间隔，不改变计算本身 |
| 跨核 DAG 依赖 | 考虑（任务级） | 保证后序任务在前序完成后才开始 |
| 子任务启动 | 独立调度 max(ready, core_free) | 模拟实际 SPMD 分发行为 |
| 调度间隔 | 置为 0 | 消除调度开销 |

## 3. 数据源映射

本分支仅包含并行式（overlapped, 1-lane, ESL_ORCH_FIRST=0）泳道数据。
折叠式（folded, 2-lane）数据将在 `onboard_orch_first` 分支提交。

### SPMD 多 tier 案例（并行式）

| 模式 (mode) | 源路径 |
|-------------|--------|
| `basic` / `double_buffer` | `proxy/spmd/<mode>/<case>/tier<N>/` |

Qwen3 案例支持 SPMD tier 0~4，对应不同任务粒度（tier 越高，单任务包含的 block 越多，
任务数越少）。每个 tier 需要单独提取 DAG。

## 4. 输出结构

```
report/swimlane/proxy_baseline/
├── basic/                              ← 标准案例
│   ├── qwen3_dynamic_manual_scope/
│   │   ├── l2_swimlane_records.json
│   │   └── l2_swimlane_trace.json
│   ├── qwen3_dynamic_tensormap/
│   ├── paged_attention_unroll/
│   └── paged_attention_unroll_manual_scope/
├── double_buffer/                      ← 标准案例
│   └── ...
└── lane2_shared/                       ← SPMD 多 tier
    ├── basic/
    │   ├── qwen3_dynamic_manual_scope/
    │   │   ├── tier0/
    │   │   ├── tier1/
    │   │   ├── tier2/
    │   │   ├── tier3/
    │   │   └── tier4/
    │   └── qwen3_dynamic_tensormap/
    └── double_buffer/
        └── ...
```

分析结果（折叠式数据，将在 `onboard_orch_first` 分支提交）：
- `report/zero_gap_analysis.json`（标准 8 组）
- `report/zero_gap_spmd_analysis.json`（SPMD 20 组）

## 5. 使用方法

### 生成标准基线泳道图

```bash
cd esl_proxy
python3 tools/zero_gap_baseline.py
```

### 生成 SPMD 多 tier 基线

```bash
python3 tools/zero_gap_baseline.py --spmd
```

### 指定 case 或 mode

```bash
python3 tools/zero_gap_baseline.py --case qwen3_dynamic_manual_scope --mode basic
```

### 跳过 trace 生成（仅生成 records）

```bash
python3 tools/zero_gap_baseline.py --no-trace
```

### 在 Perfetto 中可视化

1. 打开 https://ui.perfetto.dev/
2. 将 `l2_swimlane_trace.json` 拖入浏览器

## 6. 分析结果

> **注意**：基线分析结果（含折叠式对比）将在 `onboard_orch_first` 分支提交。
> 本分支（onboard5）仅包含并行式泳道原始数据与方法说明，不包含分析结果。

分析结果文件（`onboard_orch_first` 分支）：
- `report/zero_gap_analysis.json`（标准 8 组）
- `report/zero_gap_spmd_analysis.json`（SPMD 20 组）

## 7. 记录格式

### l2_swimlane_records.json

```json
{
  "l2_swimlane_level": 1,
  "metadata": {
    "clock_freq_hz": 50000000,
    "num_cores": 72,
    "core_types": ["aic", "aic", ..., "aiv", "aiv"],
    "baseline": true,
    "dispatch_mode": "basic|double_buffer",
    "model": "DAG-aware zero-gap, per-core order preserved (predecessor finish -> successor start, no dispatch delay)"
  },
  "aicore_tasks": [
    [core_id, task_token_raw, reg_task_id, start_cycles, end_cycles],
    ...
  ]
}
```

- `clock_freq_hz`: 50 MHz，1 cycle = 20 ns = 0.02 us
- `start_cycles` / `end_cycles`: 基线时间戳（cycles），子任务启动时间 =
  max(前驱任务完成时间, 核空闲时间)，无额外调度延迟

### l2_swimlane_trace.json

Perfetto Chrome Trace 格式，可通过 https://ui.perfetto.dev/ 可视化。
每条泳道（AIC_0 ~ AIC_23, AIV_0 ~ AIV_47）对应一个 thread，任务显示为
带颜色的色块，色块之间的间隔仅由 DAG 依赖决定。
