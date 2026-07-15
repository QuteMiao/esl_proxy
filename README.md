# esl_proxy

AICPU 编排代理：按 case 头文件构图并调度（orch / cutter / dispatch）。本机 CPU 为 Fake Return（与 `origin/main` 一致）；NPU 上板走 `platform/a2a3`。

## 四个样例

| Case | 说明 |
|------|------|
| `qwen3_dynamic_manual_scope.h` | Qwen3 全动态构图 + 任务依赖（manual scope） |
| `qwen3_dynamic_tensormap.h` | Qwen3 全动态构图 + 数据依赖（tensormap） |
| `paged_attention_unroll.h` | Paged Attention tensormap 变体 |
| `paged_attention_unroll_manual_scope.h` | Paged Attention manual scope 变体 |

## 两种运行方式

### 1. 本机 CPU（Fake Return）

不依赖 CANN。在主机上用 Makefile 跑编排 + cutter / dispatch（Fake Return，无 `platform/sim`）。

工作目录：

```bash
cd esl_proxy
```

```bash
# 默认：qwen3_dynamic_manual_scope.h，QWEN3_SPMD_TIER=2
make run

# 指定 case
make CASE=paged_attention_unroll.h run
make CASE=paged_attention_unroll_manual_scope.h run
make CASE=qwen3_dynamic_manual_scope.h QWEN3_SPMD_TIER=2 run
make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=2 run

make clean
```

产物：`esl_proxy/bin/esl_proxy`。编排统计通过 `MAIN_LOGF` 打印（`task_cnt` / `subtask_cnt` 等）。

### 2. NPU 上板（onboard）

依赖 Ascend CANN（需设置 `ASCEND_HOME_PATH`）。脚本会交叉编译 AICPU `.so`、编译 AICore ELF、构建 Host runner 并上板执行。

```bash
# 仓库根目录
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.0.0   # 按本机安装路径调整

bash tools/run_onboard.sh

bash tools/run_onboard.sh --case paged_attention_unroll_manual_scope.h -d 0
QWEN3_SPMD_TIER=2 bash tools/run_onboard.sh --case qwen3_dynamic_tensormap.h

bash tools/run_onboard.sh --skip-build --case paged_attention_unroll.h

bash tools/run_onboard.sh --all-cases
```

常用产物：

- `build/onboard/host/esl_onboard_runner`
- `build/onboard/aicpu/libaicpu_kernel.so` / `libesl_aicpu_dispatcher.so`
- `build/onboard/aicore/aicore_kernel.o`

成功时 Host 打印 `esl_proxy onboard: OK`。

## Qwen3 SPMD 档位

仅 qwen3 两个 case 生效。`QWEN3_SPMD_TIER` 取值 0…4，**默认 2**（对应 spmd-4）：

| `QWEN3_SPMD_TIER` | 等价 CLI | 目标宽度 m | 典型 task_cnt |
|------------------:|----------|------------|--------------:|
| 0 | `--non-spmd` | 1 | 3096 |
| 1 | `--spmd-2` | 2 | 1602 |
| 2（默认） | `--spmd-4` | 4 | 864 |
| 3 | `--spmd-8` | 8 | 678 |
| 4 | `--all-spmd` | total_chunks | 522 |

各档 `subtask_cnt` 均为 **3096**。paged 两个 case 无 SPMD 参数，典型 `task_cnt` / `subtask_cnt` 均为 **1920**。

```bash
make CASE=qwen3_dynamic_tensormap.h run
make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=4 run
```

## 其他 Make 选项（CPU）

```bash
make CASE=<case.h> all
make CASE=paged_attention_unroll.h WORKER_LOG=1 run
make CASE=qwen3_dynamic_tensormap.h MAIN_LOG=0 run
```

产物路径：`esl_proxy/bin/esl_proxy`。
