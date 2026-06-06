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
### Apple M5
M5 系列芯片引入了全新的命名和缓存优化，性能核心称为 “超大核”（Super Core），并对整体缓存和前端带宽（10-Wide 架构）进行了大幅度升级。

以下是搭载于 MacBook Air 的 标准款 M5 芯片 核心规格参数：

1. 核心数量与架构 (Cores)
MacBook Air 搭载的 M5 芯片提供 9 核和 10 核两种版本：
10 核版本： 4 个超大核（Super Cores）+ 6 个高能效核（Efficiency Cores）。
9 核版本： 3 个超大核（Super Cores）+ 6 个高能效核（Efficiency Cores）。

GPU 核心： 8 核或 10 核。

神经网络引擎： 16 核 Neural Engine（单核集成了独立的 Neural Accelerator，AI 推理能力大幅提升）。

1. 时钟频率 (Frequency)
超大核 (Super Core) 最高主频： 4.61 GHz
高能效核 (E-Core) 最高主频： 3.05 GHz

1. 缓存层次结构 (Cache Size)
为了大幅提升本地 LLM（大语言模型）的矩阵和张量计算效率、减少内存访问功耗，苹果显著加大了各级 Cache。

苹果 Silicon 架构由于其特殊的统一内存设计，在 CPU 层面通常不采用传统意义上的独立 L3 缓存，而是由核心集群共享的 L2 缓存 直接对接 SoC 级别的 系统级缓存（SLC，System Level Cache）。

Level 1 (L1) Cache
超大核 (Super Core)： 每个核心拥有 192 KB 指令缓存 (L1i) + 128 KB 数据缓存 (L1d)。
高能效核 (E-Core)： 每个核心拥有 128 KB 指令缓存 (L1i) + 64 KB 数据缓存 (L1d)。

Level 2 (L2) Cache
M5 的 L2 缓存按集群（Cluster）划分，各集群内部共享：

超大核集群： 4 个超大核共享高达 16 MB 的 L2 缓存（部分 9 核版本精简为约 12 MB ）。
高能效核集群： 6 个能效核共享 6 MB 至 8 MB 的 L2 缓存。

Level 3 (L3) / 系统级缓存 (SLC)
标准款 M5 芯片在整个晶圆（SoC）层面配备了约 24 MB 的系统级缓存（System Level Cache）。所有 CPU 核心、GPU 以及 Neural Engine 均可直接调用该缓存，以充当传统 CPU 的 L3 缓存角色，极大地缓解了内存总线在执行复杂指令（如 paged tensor 处理和算子融合）时的带宽压力。

截至 2026 年 6 月，Apple 公开披露的标准版 M5 芯片规格并不完整，Apple 一贯不会公开所有 Cache 层级和频率细节，因此部分数据只能依据公开资料和第三方测量推断。下面区分“官方公布”和“推测/实测”。

## 1. M5 官方公布规格

Apple 于 2025 年 10 月发布 M5。官方确认：([Apple][1])

| 项目              | M5                                     |
| --------------- | -------------------------------------- |
| 制程              | TSMC 第三代 3nm（N3P）                      |
| CPU             | 10 核                                   |
| CPU结构           | 4 Performance Core + 6 Efficiency Core |
| GPU             | 10 核                                   |
| Neural Engine   | 16 核                                   |
| 统一内存带宽          | 153 GB/s                               |
| 最大统一内存          | 32 GB                                  |
| Ray Tracing     | 第三代                                    |
| Dynamic Caching | 第二代                                    |

---

## 2. CPU频率

Apple 没有公布频率。

第三方测试数据显示：([Notebookcheck][2])

| 核心类型   | 频率            |
| ------ | ------------- |
| P-Core | 约 4.4~4.6 GHz |
| E-Core | 约 3.0 GHz     |

其中：

* P-Core峰值约 4.6GHz
* 比 M4 提升约 10~15%
* Geekbench 单核约 4100 分左右

---

## 3. Cache结构（推测）

Apple 没公布 M5 Cache。

根据 M4 与 M5 的架构连续性以及系统信息推断：

### L1 Cache

每个 Performance Core：

| 类型             | 大小     |
| -------------- | ------ |
| L1 Instruction | 192 KB |
| L1 Data        | 128 KB |

每个 Efficiency Core：

| 类型             | 大小     |
| -------------- | ------ |
| L1 Instruction | 128 KB |
| L1 Data        | 64 KB  |

与 M4 基本一致。([Reddit][3])

---

### L2 Cache

推测与 M4 相同：

| Cluster            | L2    |
| ------------------ | ----- |
| 4 × P-Core Cluster | 16 MB |
| 6 × E-Core Cluster | 4 MB  |

总计：

**20 MB L2 Cache**

([Reddit][3])

---

### System Level Cache (SLC)

Apple Silicon 没有传统 x86 意义上的 L3。

取而代之的是：

## System Level Cache (SLC)

结构：

```text
CPU L1
   ↓
CPU L2
   ↓
System Level Cache (SLC)
   ↓
Unified Memory
```

SLC 同时服务：

* CPU
* GPU
* Neural Engine
* Media Engine
* ISP

研究论文确认 Apple M 系列均存在 SLC。([arXiv][4])

标准版 M 系列历代情况：

| 芯片 | SLC          |
| -- | ------------ |
| M1 | 8 MB         |
| M2 | 8 MB         |
| M3 | 8 MB         |
| M4 | 约8~12 MB（推测） |
| M5 | 约8~12 MB（推测） |

Apple 未公开 M5 SLC 大小。

---

# 4. CPU/GPU共享机制

这是 Apple Silicon 最大特色。

## Unified Memory Architecture (UMA)

传统 PC：

```text
CPU → DDR5
GPU → GDDR6
```

两套内存。

Apple：

```text
                Unified Memory
                     ↑
   ┌──────┬──────┬──────┬──────┐
   │ CPU  │ GPU  │ NPU  │ ISP  │
   └──────┴──────┴──────┴──────┘
```

统一物理内存池。

CPU/GPU访问同一地址空间。([Apple][1])

优点：

* 零拷贝（Zero Copy）
* CPU直接访问GPU结果
* GPU直接访问CPU数据
* AI模型无需搬运数据

---

## SLC共享

Apple Silicon 中：

```text
CPU Cluster
     ↓
    L2
     ↓
   SLC
     ↑
GPU Cluster
```

CPU 和 GPU 共用 SLC。([arXiv][4])

因此：

* GPU写入纹理
* 数据进入SLC
* CPU读取时可直接命中SLC

避免回到统一内存。

---

## GPU Dynamic Caching

M5 延续 M3/M4 的 Dynamic Caching。([Apple][1])

传统GPU：

```text
固定分配缓存
```

Apple GPU：

```text
按线程组动态分配缓存
```

特点：

* 提高Cache利用率
* 减少显存访问
* 提升Ray Tracing性能

---

## GPU Neural Accelerator

M5新增：

```text
10 GPU Core
    ↓
每个GPU Core
    ↓
Neural Accelerator
```

官方称每个 GPU Core 都带有 Neural Accelerator。([Apple][1])

因此 AI 推理路径变为：

```text
CPU
 ↓
Unified Memory
 ↓
GPU Tensor/Neural Accelerator
 ↓
SLC
 ↓
Unified Memory
```

相比 M4：

* AI GPU算力提升超过4倍
* 本地LLM推理效率显著提高

---

# 5. M5缓存层级总览（当前最可信推测）

```text
P-Core
 ├─192KB L1I
 ├─128KB L1D
 └─16MB Shared L2

E-Core
 ├─128KB L1I
 ├─64KB L1D
 └─4MB Shared L2

CPU Cluster
      ↓
   8~12MB SLC
      ↓
 Unified Memory
      ↓
153GB/s
      ↓
GPU / NPU / Media Engine
```

## 总结

M5 标准版目前已确认：

* CPU：4P + 6E = 10核
* GPU：10核
* Neural Engine：16核
* 内存带宽：153 GB/s
* P-Core频率约 4.4~4.6 GHz
* L2 Cache 约 20 MB（16MB + 4MB）
* 存在共享的 System Level Cache（约 8~12 MB）
* CPU、GPU、NPU、Media Engine 通过 UMA + SLC 共享数据
* GPU 每核心新增 Neural Accelerator，成为 M5 最大架构变化之一。([Apple][1])

[1]: https://www.apple.com/newsroom/2025/10/apple-unleashes-m5-the-next-big-leap-in-ai-performance-for-apple-silicon/?utm_source=chatgpt.com "Apple unleashes M5, the next big leap in AI performance for Apple silicon - Apple"
[2]: https://www.notebookcheck.net/Apple-M5-10-Cores-Processor-Benchmarks-and-Specs.1139129.0.html?utm_source=chatgpt.com "Apple M5 (10 Cores) Processor - Benchmarks and Specs - Notebookcheck Tech"
[3]: https://www.reddit.com/r/applehelp/comments/1lki37p?utm_source=chatgpt.com "Does Apple M4 processors (10 cores) has System Level Cache ?"
[4]: https://arxiv.org/abs/2504.13385?utm_source=chatgpt.com "EXAM: Exploiting Exclusive System-Level Cache in Apple M-Series SoCs for Enhanced Cache Occupancy Attacks"
