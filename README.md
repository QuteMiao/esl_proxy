# esl_proxy

AICPU 编排 + 调度代理：算法层 + Ascend onboard/sim 平台层，支持主机 sim 与真机 NPU。

**运行方式、样例说明、最新变更与逐文件修改清单**见 [doc/overview.md](doc/overview.md)。

## 快速运行

```bash
# Sim（工作目录 esl_proxy/）
cd esl_proxy && make run

# Onboard 全样例 + 泳道图（仓库根目录，需 ASCEND_HOME_PATH）
bash tools/run_onboard.sh --all-cases --swimlane --npu
```

## 指定样例

四个 case 通过编译期宏 `ORCH_CASE` 选择（Makefile `CASE=` / onboard `--case`）。

| Case | 说明 | non-spmd task_cnt |
|------|------|------------------:|
| `qwen3_dynamic_manual_scope.h` | Qwen3 任务依赖 | 3096 |
| `qwen3_dynamic_tensormap.h` | Qwen3 tensormap | 3096 |
| `paged_attention_unroll.h` | Paged Attention tensormap | 1920 |
| `paged_attention_unroll_manual_scope.h` | Paged Attention manual scope | 1920 |

### Sim

工作目录 `esl_proxy/`：

```bash
# ── 基础选项 ──
make run                              # 默认 case + instant AICore
make clean && make all && ./bin/esl_proxy
make CASE=qwen3_dynamic_manual_scope.h all   # 仅编译

# ── 运行所有样例 / 设置样例 ──
# 四样例依次 benchmark（10 次中位数，仓库根目录）
bash ../tools/run_sim_benchmark.sh

make CASE=qwen3_dynamic_tensormap.h run
make CASE=paged_attention_unroll_manual_scope.h run

# ── 开启日志 ──
make run WORKER_LOG=1                 # 运行时 worker CSV（编译期 WORKER_LOG=1）
make CASE=qwen3_dynamic_tensormap.h MAIN_LOG=0 run          # 关闭主线程统计
make CASE=qwen3_dynamic_tensormap.h MAIN_LOG=0 WORKER_LOG=1 run

# ── 开启双缓冲 ──
make DISPATCH=double_buffer run
make clean && make CASE=paged_attention_unroll.h DISPATCH=double_buffer all && ./bin/esl_proxy

# ── 设置 SPMD 参数（仅 qwen3 两个 case，编译期）──
make CASE=qwen3_dynamic_manual_scope.h QWEN3_SPMD_TIER=0 run   # non-spmd（默认，3096 task）
make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=2 run        # spmd-4 tier（864 task）

# ── 其他 ──
make SIM_AICORE=threaded run          # 72 pthread + fake_kernel 时序 sim
make CASE=paged_attention_unroll.h QWEN3_SPMD_TIER=0 run
```

### Onboard

仓库根目录：

```bash
# 指定单个 case
bash tools/run_onboard.sh --case qwen3_dynamic_manual_scope.h
bash tools/run_onboard.sh --case paged_attention_unroll.h --swimlane

# 全部四个 case（含泳道图 → report/swimlane/basic/ 或 double_buffer/）
bash tools/run_onboard.sh --all-cases --swimlane
bash tools/run_onboard.sh --all-cases --swimlane --double-buffer
bash tools/run_onboard.sh --all-cases --swimlane --npu
bash tools/run_onboard.sh --all-cases --swimlane --double-buffer --npu

# 环境变量等价于 --case
ESL_PROXY_ORCH_CASE=qwen3_dynamic_tensormap.h bash tools/run_onboard.sh --swimlane
```

Qwen3 SPMD 档位：`QWEN3_SPMD_TIER=0..4`（默认 0）。详见 [doc/overview.md#qwen3-spmd](doc/overview.md#qwen3-spmd)。

## 测试报告

- Sim 基准（10 次中位数）：`bash tools/run_sim_benchmark.sh` → `report/sim_benchmark.json`
- 上板泳道图：`report/swimlane/basic/` 与 `report/swimlane/double_buffer/`
- 汇总报告：[report/benchmark_report.md](report/benchmark_report.md)
