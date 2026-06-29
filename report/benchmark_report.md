# esl_proxy 基准测试报告

生成时间：2026-06-29  
条件：`QWEN3_SPMD_TIER=0`（non-spmd），四样例全部 **PASS**

---

## 1. Sim（主机 instant AICore）

方法：每样例 **10 次**运行取**中位数**（`tools/run_sim_benchmark.sh`）  
配置：`SIM_AICORE=instant`，`DISPATCH=basic`

| 样例 | task_cnt | Orch TP (MTasks/s) | Sched TP (MTasks/s) | Sched 耗时 (ms) |
|------|---------:|-------------------:|--------------------:|----------------:|
| qwen3_dynamic_manual_scope | 3096 | 3.25 | 2.48 | 1.26 |
| qwen3_dynamic_tensormap | 3096 | 1.55 | 1.47 | 2.12 |
| paged_attention_unroll | 1920 | 2.32 | 2.25 | 0.85 |
| paged_attention_unroll_manual_scope | 1920 | 3.75 | 3.61 | 0.53 |

原始数据：[sim_benchmark.json](sim_benchmark.json)

---

## 2. 上板 NPU — 单缓冲（basic dispatch）

命令：`bash tools/run_onboard.sh --all-cases --swimlane --npu`

泳道图目录：`report/swimlane/basic/<case>/`

| 样例 | task_cnt | wall (ms) | span (ms) | ktasks/s | 泳道图 |
|------|---------:|----------:|----------:|---------:|--------|
| qwen3_dynamic_manual_scope | 3096 | 9.63 | 6.35 | 487.35 | [trace](swimlane/basic/qwen3_dynamic_manual_scope/l2_swimlane_trace.json) |
| qwen3_dynamic_tensormap | 3096 | 8.96 | 5.53 | 559.56 | [trace](swimlane/basic/qwen3_dynamic_tensormap/l2_swimlane_trace.json) |
| paged_attention_unroll | 1920 | 6.82 | 3.43 | 558.96 | [trace](swimlane/basic/paged_attention_unroll/l2_swimlane_trace.json) |
| paged_attention_unroll_manual_scope | 1920 | 6.74 | 3.56 | 538.61 | [trace](swimlane/basic/paged_attention_unroll_manual_scope/l2_swimlane_trace.json) |

日志：[onboard_basic_run.log](../onboard_basic_run.log)

---

## 3. 上板 NPU — 双缓冲（double_buffer dispatch）

命令：`bash tools/run_onboard.sh --all-cases --swimlane --double-buffer --npu`

泳道图目录：`report/swimlane/double_buffer/<case>/`

| 样例 | task_cnt | wall (ms) | span (ms) | ktasks/s | 泳道图 |
|------|---------:|----------:|----------:|---------:|--------|
| qwen3_dynamic_manual_scope | 3096 | 8.68 | 5.82 | 532.00 | [trace](swimlane/double_buffer/qwen3_dynamic_manual_scope/l2_swimlane_trace.json) |
| qwen3_dynamic_tensormap | 3096 | 8.89 | 5.36 | 577.61 | [trace](swimlane/double_buffer/qwen3_dynamic_tensormap/l2_swimlane_trace.json) |
| paged_attention_unroll | 1920 | 6.41 | 3.20 | 599.93 | [trace](swimlane/double_buffer/paged_attention_unroll/l2_swimlane_trace.json) |
| paged_attention_unroll_manual_scope | 1920 | 5.87 | 3.29 | 584.11 | [trace](swimlane/double_buffer/paged_attention_unroll_manual_scope/l2_swimlane_trace.json) |

日志：[onboard_double_buffer_run.log](../onboard_double_buffer_run.log)

---

## 4. 单缓冲 vs 双缓冲（上板 wall 时间）

| 样例 | basic wall (ms) | double_buffer wall (ms) | 变化 |
|------|----------------:|------------------------:|-----:|
| qwen3_dynamic_manual_scope | 9.63 | 8.68 | **-9.9%** |
| qwen3_dynamic_tensormap | 8.96 | 8.89 | -0.8% |
| paged_attention_unroll | 6.82 | 6.41 | **-6.0%** |
| paged_attention_unroll_manual_scope | 6.74 | 5.87 | **-12.9%** |

双缓冲在四个样例上 wall 时间均有下降；paged manual_scope 收益最大（约 13%）。

---

## 5. 泳道图查看

每个 case 目录仅保留 **`l2_swimlane_trace.json`**（Perfetto 格式，拖入 https://ui.perfetto.dev/）。

```
report/swimlane/
├── basic/
│   ├── qwen3_dynamic_manual_scope/
│   ├── qwen3_dynamic_tensormap/
│   ├── paged_attention_unroll/
│   └── paged_attention_unroll_manual_scope/
└── double_buffer/
    ├── qwen3_dynamic_manual_scope/
    ├── qwen3_dynamic_tensormap/
    ├── paged_attention_unroll/
    └── paged_attention_unroll_manual_scope/
```

---

## 6. 复现命令

```bash
# Sim 基准（10 次中位数）
bash tools/run_sim_benchmark.sh

# 上板 basic + 泳道图
QWEN3_SPMD_TIER=0 bash tools/run_onboard.sh --all-cases --swimlane --npu

# 上板 double_buffer + 泳道图
QWEN3_SPMD_TIER=0 bash tools/run_onboard.sh --all-cases --swimlane --double-buffer --npu
```
