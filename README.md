# esl_proxy

AICPU 编排（Orchestrator）代理：编译时选择 case 头文件，运行编排逻辑并输出 task / subtask 统计。当前默认**仅跑编排**，不启动 Scheduler（dispatch / cutter 线程）。

## 构建与运行

工作目录：

```bash
cd esl_proxy
```

### 四个样例

| Case | 说明 |
|------|------|
| `qwen3_dynamic_manual_scope.h` | Qwen3 全动态构图 + 任务依赖（manual scope） |
| `qwen3_dynamic_tensormap.h` | Qwen3 全动态构图 + 数据依赖（tensormap） |
| `paged_attention_unroll.h` | Paged Attention tensormap 变体 |
| `paged_attention_unroll_manual_scope.h` | Paged Attention manual scope 变体 |

```bash
# 默认：qwen3 manual scope，QWEN3_SPMD_TIER=2（spmd-4）
make run

# Qwen3 manual scope
make CASE=qwen3_dynamic_manual_scope.h run

# Qwen3 tensormap
make CASE=qwen3_dynamic_tensormap.h run

# Paged Attention tensormap
make CASE=paged_attention_unroll.h run

# Paged Attention manual scope
make CASE=paged_attention_unroll_manual_scope.h run
```

### Qwen3 SPMD 档位

仅 qwen3 两个 case 生效。`QWEN3_SPMD_TIER` 取值 0…4，**默认 2**（对应 V200-benchmark `--spmd-4`，目标宽度 m=4）：

| `QWEN3_SPMD_TIER` | 等价 CLI | 目标宽度 m | 典型 task_cnt |
|------------------:|----------|------------|--------------:|
| 0 | `--non-spmd` | 1 | 3096 |
| 1 | `--spmd-2` | 2 | 1602 |
| 2（默认） | `--spmd-4` | 4 | 864 |
| 3 | `--spmd-8` | 8 | 678 |
| 4 | `--all-spmd` | total_chunks | 522 |

各档 kernel 下发次数（batch=90 → batch_padded=96，Scope1/3 各 6 个 tile、Scope2 逐 batch；不含 alloc 等框架开销 task）：

| 档位 | Scope1 | Scope2 | Scope3 | 合计下发次数 |
|------|-------:|-------:|-------:|------------:|
| `--non-spmd` | 228 | 1530 | 1338 | **3096** |
| `--spmd-2` | 120 | 810 | 672 | **1602** |
| `--spmd-4` | 66 | 450 | 348 | **864** |
| `--spmd-8` | 42 | 450 | 186 | **678** |
| `--all-spmd` | 30 | 450 | 42 | **522** |

注：Scope2 在 `--spmd-4` 及以上不再变化（注意力四算子已封顶为 4，rope 为单任务）。合计下发次数即编排输出的 `task_cnt`。

```bash
# 使用默认 tier=2，无需显式指定
make CASE=qwen3_dynamic_tensormap.h run

# 显式指定档位
make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=4 run
make CASE=qwen3_dynamic_manual_scope.h QWEN3_SPMD_TIER=0 run
```

各档 `subtask_cnt` 均为 **3096**（物理子任务总数不变；SPMD 只改变 task 下发次数）。paged 两个 case 无 SPMD 档位参数，典型 `task_cnt` / `subtask_cnt` 均为 **1920**。

### 其他 Make 目标与选项

```bash
make clean                                    # 清理 build/、bin/
make CASE=<case.h> all                        # 只编译不运行
make test-dep-dump                            # 依赖边 dump 单元测试

# 运行时选项
make CASE=qwen3_dynamic_tensormap.h DEP_DUMP=1 run   # 编排后导出依赖边
make CASE=qwen3_dynamic_tensormap.h NO_DEPS=1 run    # 无依赖编排基准
make CASE=paged_attention_unroll.h WORKER_LOG=1 run  # 开启 worker 日志
make CASE=qwen3_dynamic_tensormap.h MAIN_LOG=0 run # 关闭主线程编排统计输出
```

**`MAIN_LOG`**：编译期开关，控制 `main` 线程的 `MAIN_LOGF` 是否输出到终端。`conf.h` 默认为 `1`，因此直接 `make run` 会打印编排统计（`task_cnt`、`subtask_cnt`、耗时、吞吐等）。设为 `0` 可静默运行；显式写 `MAIN_LOG=1` 与默认行为相同。

产物路径：`bin/esl_proxy`。

## 性能出口
### 基础性能

### 业务性能

## 测试平台
