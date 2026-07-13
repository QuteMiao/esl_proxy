# esl_proxy 概览

AICPU 编排 + 调度代理：在算法层之上接入 Ascend onboard/sim 平台层，支持主机 sim 与真机 NPU 两套运行路径。

相对历史 `main` 分支（仅编排 + fake-return sim）的变更说明见下文各节。

---

## 快速开始

### 目录结构

| 路径 | 说明 |
|------|------|
| `esl_proxy/` | 源码、Makefile、`cases/`、`include/` |
| `tools/run_onboard.sh` | onboard 一键构建 + 运行 |
| `cmake/` | AICPU / AICore / host 三套 CMake 构建 |

### Sim（主机 pthread 模拟）

工作目录：`esl_proxy/`（即含 Makefile 的子目录）。

```bash
cd esl_proxy

# 默认：qwen3 manual scope，instant AICore，QWEN3_SPMD_TIER=0
make clean && make all && ./bin/esl_proxy

# 等价
make run

# 指定 case（SPMD 关闭示例）
make CASE=qwen3_dynamic_manual_scope.h QWEN3_SPMD_TIER=0 run
make CASE=paged_attention_unroll_manual_scope.h run
```

**Sim 编译开关**

| 变量 | 默认 | 说明 |
|------|------|------|
| `CASE` | `qwen3_dynamic_manual_scope.h` | 编译进 `main.c` 的编排头文件 |
| `QWEN3_SPMD_TIER` | `0` | 仅 qwen3 两个 case；0=non-spmd |
| `SIM_AICORE` | `instant` | `instant`：1 个 manager 线程即时 FIN；`threaded`：72 pthread + fake_kernel |
| `DISPATCH` | `basic` | `double_buffer`：双缓冲 |
| `MAIN_LOG` | `1` | 主线程编排/调度统计输出 |
| `WORKER_LOG` | 运行时 `WORKER_LOG=1` | worker CSV 日志 |

```bash
make SIM_AICORE=threaded run
make clean && make all DISPATCH=double_buffer && ./bin/esl_proxy
make CASE=qwen3_dynamic_tensormap.h QWEN3_SPMD_TIER=0 MAIN_LOG=0 run
```

产物：`esl_proxy/bin/esl_proxy`。成功时打印 `[host] PASS: task_cnt=...`。

### Onboard（Ascend NPU）

工作目录：仓库根目录（含 `tools/`）。

```bash
# 需 ASCEND_HOME_PATH
export ASCEND_HOME_PATH=/path/to/cann

# 默认 case 构建 + 运行
bash tools/run_onboard.sh

# 常用选项
bash tools/run_onboard.sh --case qwen3_dynamic_manual_scope.h
bash tools/run_onboard.sh --double-buffer          # 双缓冲 dispatch
bash tools/run_onboard.sh --all-cases              # 四个 case 依次跑
bash tools/run_onboard.sh --swimlane               # L2 swimlane trace
bash tools/run_onboard.sh --npu                     # task-submit 上真机
bash tools/run_onboard.sh --skip-build             # 复用已有 build
```

环境变量：`QWEN3_SPMD_TIER=0..4`（默认 0）、`ESL_PROXY_ORCH_CASE=<case>.h`。

源码列表：`cmake/sources.cmake`（AICPU kernel）；sim 列表在 `esl_proxy/Makefile`。

---

## 四个样例

| Case | task_cnt（non-spmd） | 说明 |
|------|---------------------:|------|
| `qwen3_dynamic_manual_scope.h` | 3096 | Qwen3 任务依赖 |
| `qwen3_dynamic_tensormap.h` | 3096 | Qwen3 tensormap 数据依赖 |
| `paged_attention_unroll.h` | 1920 | Paged Attention tensormap |
| `paged_attention_unroll_manual_scope.h` | 1920 | Paged Attention manual scope |

## Qwen3 SPMD {#qwen3-spmd}

仅 `qwen3_dynamic_*.h` 两个 case 生效。默认 **`QWEN3_SPMD_TIER=0`**（non-spmd，task_cnt=3096）。

| tier | task_cnt |
|-----:|---------:|
| 0 | 3096 |
| 1 | 1602 |
| 2 | 864 |
| 3 | 678 |
| 4 | 522 |

paged 两个 case 无 SPMD 参数，task_cnt / subtask_cnt 均为 **1920**。

---

## 最新提交摘要（`5d65c41`，相对 `main`）

1. **目录重组**：算法代码迁入 `include/algorithm/`、`src/algorithm/`；平台代码分 `platform/sim/` 与 `platform/onboard/`。
2. **端到端 sim**：`main.c` 启动 platform 初始化 + cutter/dispatch + AICore 握手/调度，不再使用 fake-return 假完成。
3. **Sim instant AICore**（默认）：单 manager 线程轮询全部 core 并即时 ACK/FIN；`SIM_AICORE=threaded` 保留 72 pthread 旧模式。
4. **Onboard 构建链**：`cmake/` + `tools/run_onboard.sh` 统一 AICPU kernel / AICore / host 构建与运行。
5. **L2 swimlane**：可选 trace 导出与 `swimlane_trace.py` 分析。
6. **算法加固**：cache 维护、显式 memory order、`NODE_BUFF_SIZE` 65536、静态 predecessor 缓冲、AICPU–AICore handshake 状态机。

---

## algorithm/ 模块改动

路径均相对于 `esl_proxy/include/algorithm/` 与 `esl_proxy/src/algorithm/`。按运行时职责划分；各模块通过 `ring_buf` / `ctrl_t` / `g_runtime` 衔接。

```
编排(case) → ring_buf/new_task → cutter(构图) → ready_queue
                                              ↓
                                    dispatch(下发+回收) → AICore ACK/FIN
                                              ↓
                                    completed_queue → cutter 继续推进
```

### 编排与任务描述（Ring Buffer / Task）

**职责**：主线程（`cases/*.h`）提交 task 描述符、维护前驱环与 task_id 生命周期。

| 文件 | 改动 |
|------|------|
| **ring_buf.h** | 前驱环 backing 改为静态 `predecessor_storage[]`（见 shm.c）；`new_task()` 增加 `duration_ns` / `jitter_mask`，写 `type` / `id` / `count` / `mode`；`count > 1` 时标 `ORG_MODE_SPMD_SYNC` 并累加 `g_subtask_cnt`；新增 **`advance_task_id()`**（wmb + release）；前驱追加后 `wmb()`；引入 `platform.h` / `memory_barrier.h` |
| **task.h** | `task_desc` 对齐 onboard：`duration`（ns）、`jitter_mask`、`mode`（SINGLE/GROUP/SPMD_*）、`count`（block_num） |
| **shm.c** | 落地 **`predecessor_storage[]`**、静态 **`g_state_buf[]`**、weak **`g_subtask_cnt`** |
| **conf.h** | 迁入 `algorithm/`；`AIC_CNT` ← `ESL_PROXY_WORKER_BLOCK_DIM`；`NODE_BUFF_SIZE` **65536**；`CON_NODE_CNT` 256 |

相对 main：`advance_task_id` 拆分提交与可见性；编排不再 malloc 前驱节点。

### Cutter（构图 / 依赖解析）

**职责**：消费 `completed_queue`，解析前驱，把就绪 task 放入 `ready_queue[cube|vector]`，维护 `g_state_buf` / `g_commit_task_id`。

| 文件 | 改动 |
|------|------|
| **cutter.h** | `g_state_buf` 指针 → **静态数组**；`g_commit_task_id` → `_Atomic uint16_t` |
| **cutter.c** | commit 批次前 **`cache_civac_lines` + barrier**；`update_task_state` / `resolve_dep` 加 `wmb()`；主循环 **`spin_wait()`** + acquire fence；drain 直到 commit 追上 `g_task_id` |
| **queue.h** | MPMC 队列 memory order 与 include 路径调整 |

**限制**：MIX 专用队列（`ready_queue[2]`）未实现，type 仍直映射 cube/vector 下标。

### Dispatch（任务下发 / 完成回收）

**职责**：dispatch worker 线程从 `ready_queue` 取就绪 task，经 MMIO 下发至 AICore，poll COND 寄存器回收完成，写 `msg_bitmap` → `completed_queue` → `g_completed_cnt`。sim / onboard **共用源码**，依赖 `g_runtime` + `platform_regs.h`。

#### 数据流

```
ready_queue ──► send_task / prefetch ──► esl_prepare_subtask_to_core (512B payload)
                        │                      write REG DATA_MAIN_BASE (门铃)
                        ▼
                 dispatch_poll ◄────── COND: ACK / FIN
                        │
                        ▼
              msg_bitmap → completed_queue → g_completed_cnt
```

#### 源码与构建

| 文件 | 职责 |
|------|------|
| **dispatch.h** | `ctrl_t`（free/msg bitmap、ready/completed 队列）；`dispatch_worker` / `dispatch_poll`；`extern EslRuntime *g_runtime` |
| **dispatch.c** | **basic**（默认）：每 logical core 单 slot 在飞；poll 独立判 FIN |
| **dispatch_double_buffer.c** | **双缓冲**：每核 2-outstanding + prefetch；COND 覆盖下 running/pending 推断 |
| **dispatch_payload.c** | 两变体共享：payload 组装、per-core reg task_id、`esl_publish_subtask_to_core` |
| **runtime.h** | `EslRuntime`、`EslDispatchPayload`（512B）、`esl_prepare/publish_subtask_to_core` 声明 |

构建：`Makefile` 的 `DISPATCH=basic|double_buffer` 与 cmake `ESL_PROXY_DOUBLE_BUFFER` 二选一链接 dispatch 变体；**`dispatch_payload.c` 始终链接**。

#### 相对 main 的核心变化

| 项 | main（旧） | 当前 |
|----|------------|------|
| 完成判定 | dispatch 内 fake-return | poll `platform_reg_task_finished` / COND |
| 任务下发 | 仅写 executor 本地状态 | payload build + MMIO publish |
| 物理核映射 | logical core = worker | `platform_pick_phys_worker` → 72 路 AIC/AIV |
| cache | 无 | publish 前 civac task_desc / 前驱环 |
| 空 slot | `0` | **`EXEC_SLOT_EMPTY`**（0xFFFF） |
| 统计 | 本地 log | `platform_stats_publish`（onboard → device_wall） |

#### basic（`dispatch.c`）

1. **`dispatch()` 每轮**：`get_free_exe` 合并 msg→free bitmap → `push_2_completed_queue` 批量入 completed 队列 → 按 MIX / VECTOR / CUBE 调 **`send_task`**。
2. **`send_task`**：按 `free_bitmap[type][0] & free_bitmap[type][1]` 找空闲 (core, slot)；civac 任务元数据；写 `g_executors`；`platform_pick_phys_worker` 得 phys index；`esl_prepare_subtask_to_core(..., block_idx=0)` → 写 reg → opportunistic **`dispatch_mark_slot_complete`**（instant sim 可当场 FIN）。
3. **`dispatch_poll`**：扫描在飞 slot，读 phys core COND，FIN 则 ack + 置 msg_bitmap + 清空 slot。
4. **`dispatch_worker` 主循环**：编排期间 + drain 阶段循环 `dispatch` → `poll` → `spin_wait()`，直至 `g_completed_cnt == g_task_id`；打印 scheduler TP；**`dispatch_publish_final_stats`**。
5. **sim 特化**：basic 将 duration **÷10000** 加速 threaded fake_kernel（double_buffer 不缩放）。

#### 双缓冲（`dispatch_double_buffer.c`）

在 basic 上增加 **每 logical core 两路在飞**，掩盖 AICore 延迟（上板 wall 时间可降 ~6–13%，见 benchmark 报告）。

1. **`send_task`**：与 basic 相同，发第一个 slot。
2. **`dispatch_prefetch`**（basic 无）：core **一忙一闲** 且 busy 已 **ACK** 时，向 free slot 预发第二任务（VECTOR/CUBE 分别 prefetch）。
3. **`dispatch_poll` 增强**：
   - 同 phys core 两 slot：`platform_reg_cond_raw` 一次读 COND，按 reg_task_id 序分 running / pending；
   - **串行推断**：见 pending ACK/FIN ⇒ running 已完成 → **`dispatch_force_complete`**；
   - 不同 phys core（AIV lane 轮替）仍 per-slot 独立判 FIN。
4. **`dispatch()` 末尾**：在 send_task 之后追加 vector/cube **prefetch**。

#### Payload 路径（`dispatch_payload.c`）

- 每 worker **`EslHandshake.task`** 指向 **2×512B** payload；`reg_task_id & 1` 选 slot。
- **`build_payload`**：`function_bin_addr`（AIC/AIV fake kernel）、`args[0/1]`（duration_ns / jitter_mask）、`async_task_token`（编排 task_id）、`local_block_num = desc->count`。
- **`dispatch_next_reg_task_id`**：per-core 递增，避开 `AICORE_EXIT_SIGNAL`。
- **`esl_init_global_context`**：初始化 AIV `global_sub_block_id`。
- **publish**：`write_reg(REG_ID_DATA_MAIN_BASE, reg_task_id)` 敲门铃。

#### 与相邻模块的接口

- **cutter** → `ready_queue`；dispatch 不解析 DAG。
- **handshake** → `dispatch_core_reg_addr` 取 MMIO 基址。
- **swimlane** → publish 时 `ESL_SWIMLANE_AICPU_ON_DISPATCH`。
- **executor.h** → `g_executors` / `EXEC_SLOT_EMPTY` 仍作 slot 状态机（executor 线程本身未启用）。

#### Dispatch 已知限制

- MIX 队列未独立；MIX 靠 cube∩vector free bitmap 模拟。
- Work-stealing TODO 未实现。
- **SPMD**：`block_idx` 恒 0，未按 `count` 多次 publish（见 [已知限制](#已知限制)）。

```bash
# Sim
make DISPATCH=double_buffer run

# Onboard
bash tools/run_onboard.sh --double-buffer
```

### Handshake（AICPU–AICore 握手）

**职责**：bootstrap 全核 reg 表、等 AICore ready、shutdown 时两阶段 EXIT，避免 sim 死循环。

| 文件 | 改动 |
|------|------|
| **handshake.h** | **新增**。`esl_handshake_all_cores` / `esl_shutdown_all_cores` / `esl_handshake_reg_addr` |
| **handshake.c** | **新增**。全核 bootstrap；**Phase1** 广播 EXIT；**Phase2** 等 COND `EXITED` 再清 IDLE |

platform init 调用 handshake；dispatch 通过 `esl_handshake_reg_addr` 解析 reg 基址。

### AICore 执行体

**职责**：AICore 侧读 payload、执行 kernel、写 COND ACK/FIN；替代 main 时代 executor 线程的 fake 计时路径。

| 文件 | 改动 |
|------|------|
| **aicore_executor.h / .c** | **新增**。`aicore_execute` 弱符号：握手 → 门铃循环 → `fake_kernel_run` → FIN |
| **fake_kernel.h** | **新增**。sim threaded / onboard stub kernel 函数 id |
| **executor.h / .c** | 新增 **`EXEC_SLOT_EMPTY`**；`executor_worker` 内 SPMD duration 循环 **保留但未启线程**（main 不 create） |

sim **instant** 模式由 `device_runner_instant.c` 代写 ACK/FIN，不经 `aicore_execute` 计时。

### 内存池（Mem Pool / Manager）

**职责**：编排阶段 O(1) 分配 tensor 缓冲；when2free 自动回收（manager 线程轮询）。

| 文件 | 改动 |
|------|------|
| **mem_pool.h** | 迁入 `algorithm/`，逻辑同 main |
| **manager.h / manager.c** | 路径迁移；**sim/onboard 均未链接** manager 线程（QuteMiao 原版保留） |

sim：`main.c` 栈上 **1GB** `g_mem_pool_storage` + `mem_pool_init`；onboard：设备 GM 64GB 池。

### Tensormap / Tensor

**职责**：qwen3 tensormap case 的数据依赖推断；tensor 视图与 `TmEntry` 布局。

| 文件 | 改动 |
|------|------|
| **tensor.h / tensormap.h / tensormap_core.h** | 迁入 `include/algorithm/`，include 路径更新；与 main 逻辑一致 |

### 公共原语

| 文件 | 改动 |
|------|------|
| **spin.h / mpmc_queue.h** | 迁入 `algorithm/`，cutter/dispatch 自旋与队列原语 |

---

## 框架层修改（algorithm 以外）

### 入口与构建

| 路径 | 修改点 |
|------|--------|
| **esl_proxy/src/main.c** | Sim：增加 **`esl_platform_init/shutdown`**；cutter/dispatch 线程保留；编排后 **PASS/FAIL** 校验 `g_completed_cnt == g_task_id`；onboard host 分支 `#if ESL_PROXY_ONBOARD_HOST` 转调 `esl_onboard_run()`。 |
| **esl_proxy/Makefile** | C++ 链接；`-I include/algorithm|platform|sim|onboard|swimlane`；**`SIM_AICORE=instant|threaded`**；**`DISPATCH=basic|double_buffer`**；默认 `QWEN3_SPMD_TIER=0`；移除 `manager.c`/trace stub/`asm` 目标。 |
| **cmake/** | **新增** AICPU / AICore / host 三套 CMakeLists；**sources.cmake** 共享 algorithm + onboard 源列表；支持 `ESL_PROXY_DOUBLE_BUFFER`。 |
| **tools/run_onboard.sh** | **新增**。内联原 build_aicpu/aicore/host + swimlane 批跑；`--double-buffer`、`--all-cases`、`--swimlane`、`--npu`。 |
| **tools/swimlane_trace.py** | **新增**。L2 trace 解析与 perf 汇总。 |
| **tools/gen_all_dags.sh** | **新增**。批量 DAG 生成。 |

### Platform 层结构（自顶向下）

Platform 层位于 **algorithm**（cutter / dispatch / handshake 等调度逻辑）与 **物理执行环境**（主机 pthread 模拟 或 Ascend NPU）之间。设计思路是 **先定契约、再选后端、最后填实现**：algorithm 只依赖 `platform.h` 等公共头，不感知 sim/onboard 差异；Makefile / CMake 在链接阶段选择 `platform/sim/` 或 `platform/onboard/` 源文件。

```
                    ┌─────────────────────────────────────┐
  algorithm 层      │ cutter / dispatch / handshake / …   │
                    └─────────────────┬───────────────────┘
                                      │ 只 include platform/*.h
                    ┌─────────────────▼───────────────────┐
  L0 统一 HAL       │ platform.h — init/shutdown、cache、  │
                    │ stats、handshake bootstrap           │
                    └─────────────────┬───────────────────┘
                    ┌─────────────────▼───────────────────┐
  L1 公共契约       │ worker_map / platform_config /       │
                    │ platform_regs / memory_barrier / log │
                    └─────────┬───────────────┬───────────┘
                              │               │
              ┌───────────────▼──┐    ┌───────▼──────────────┐
  L2 后端实现   │  platform/sim   │    │  platform/onboard     │
              │  主机 pthread    │    │  CANN AICPU + NPU HAL │
              └───────────────┬──┘    └───────┬──────────────┘
                              │               │
              ┌───────────────▼──┐    ┌───────▼──────────────┐
  L3 设备执行   │ device_runner  │    │ aicpu_runtime +       │
              │ (instant/thread)│    │ aicore_entry + HAL    │
              └────────────────┘    └───────────────────────┘
```

#### L0 — 统一 HAL 入口（`include/platform/platform.h`）

algorithm 与 main 唯一需要关心的 platform 面：

| 接口 | 职责 |
|------|------|
| `esl_platform_init` / `esl_platform_shutdown` | 运行时 bring-up / teardown（reg 表、ring_buf、handshake、AICore worker） |
| `platform_handshake_aicore_bootstrap` | 启动前预填 sim 侧 ACK 字段；onboard 为 no-op（真核自行写 reg） |
| `cache_*` | 调度快照与 payload 的 cache 维护抽象 |
| `platform_stats_publish` | dispatch 结束时上报统计；onboard 写 device_wall，sim 为 no-op |
| `platform_pick_phys_worker` | logical core → 物理 worker（`platform.h` inline，委托 `worker_map.h::esl_pick_phys_worker`） |
| AICPU role 常量 | **仅** `platform_config.h`（`ESL_AICPU_ROLE_CUTTER/DISPATCH/ORCH`） |

Sim 的 `main.c` 在调用 `esl_platform_init` 前自行完成 **1GB mem_pool 栈分配**；onboard 则在 AICPU 侧 `platform_init.c` 映射设备内存池。

#### L1 — 公共契约（sim / onboard 共用头文件）

这一层把 **拓扑、寄存器布局、并发原语** 定死，两套后端必须语义一致，algorithm 才能共用同一份源码。

| 头文件 | 定什么 |
|--------|--------|
| **worker_map.h** | **拓扑 SSOT**：`ESL_PROXY_WORKER_BLOCK_DIM` / `ESL_PROXY_HOST_WORKER_COUNT` / `esl_pick_phys_worker`；`platform.h`、`platform_config.h`、`runtime.h` 仅做别名，禁止再写死 24/72 |
| **platform_config.h** | RegId 枚举、512B `EslDispatchPayload` 布局、task_id 编码、profiling flag |
| **platform_regs.h** | `read_reg` / `write_reg` / COND·ACK·FIN 判定；声明 cache 原语，**定义在各后端 .c** |
| **memory_barrier.h / thread_yield.h** | WMB/RMB 与 spin yield；sim 用编译器 barrier + `sched_yield`，onboard 用 aarch64 指令 |
| **log.h** | 统一日志宏；后端分别链 `sim/log.c` 或 `onboard/onboard_log.c` |

#### L2 — 后端选择与生命周期

构建时二选一链接，**同名符号、不同实现**：

| | **sim**（`src/platform/sim/`） | **onboard**（`src/platform/onboard/`） |
|---|-------------------------------|----------------------------------------|
| 入口 | `main.c` → `esl_platform_init` | `aicpu_runtime.c`（AICPU kernel）或 `host_onboard.c`（Host 加载） |
| 初始化 | `platform_init.c`：runtime layout、ring_buf、handshake、启 AICore worker | `platform_init.c`：64GB 设备 mem_pool、swimlane hook、等真核 handshake |
| MMIO | `platform_regs.c` + `sim_core_regs.c`：host 内存模拟 reg 表 | `npu_hal.c`：真 MMIO + aarch64 cache |
| cache | `cache_ops.c`：civac/dsb 空操作或 host 等价 | `cache_ops.c`：dc civac / cvac |
| 日志 | `log.c` | `onboard_log.c`（CANN dlog） |

#### L3 — 设备执行体（AICore 侧 ACK/FIN 的来源）

algorithm 的 dispatch 写 payload + 敲门铃；**谁消费任务、谁写 FIN** 由本层决定——这是相对历史 main「dispatch 内 fake-return」的核心变化。

**Sim 两条路径**（`SIM_AICORE=instant|threaded`）：

| 组件 | instant（默认） | threaded |
|------|----------------|----------|
| 调度器 | `device_runner_instant.c`：单线程轮询 72 核，收任务即 ACK+FIN | `device_runner.c`：72× pthread |
| 执行体 | 无 kernel，零延迟 FIN | `aicore.c` + `aicore_wrapper.cpp`，fake_kernel + duration 递减 |
| 共享 | `handshake.c` 两阶段 shutdown；`aicore_executor.c` 读 payload 写 COND |

**Onboard 路径**：

| 组件 | 职责 |
|------|------|
| `aicpu_runtime.c` | CANN AICPU 入口：orch + cutter + dispatch 线程、CPU affinity、handshake |
| `aicore_entry.cpp` | AICore kernel 入口，链 `aicore_executor.c` 设备侧循环 |
| `host_onboard.c` | Host 侧加载 kernel、ioctl、跑编排 case、收集泳道 trace |
| `tools.c` / `aicpu_affinity.c` | 设备辅助 API 与线程绑核 |

#### 目录速查

```
include/platform/
├── platform.h / platform_config.h / platform_regs.h   # L0–L1 公共契约
├── worker_map.h / memory_barrier.h / thread_yield.h / log.h
├── sim/          # sim 专用头（device_runner、sim_core_regs、aicore）
└── onboard/      # onboard 专用头（aicpu_runtime、kernel_args、onboard_config）

src/platform/
├── sim/          # platform_init, device_runner[_instant], platform_regs, aicore, cache_ops, log
└── onboard/      # aicpu_runtime, aicore_entry, npu_hal, host_onboard, cache_ops, onboard_log
```

### swimlane/

| 路径 | 修改点 |
|------|--------|
| **swimlane_*.h / swimlane_aicpu.c / host_swimlane.c** | L2 级 AICPU/AICore/host trace hook，可选编译 `ESL_PROXY_ENABLE_L2_SWIMLANE`。 |

### cases/

| 路径 | 修改点 |
|------|--------|
| **qwen3_dynamic_*.h** | `new_task` 适配 ns duration + jitter；tensormap 路径对齐 `advance_task_id`；删 `org.h`。 |
| **paged_attention_*.h** | 同上 task 提交 API；include 路径改为 `algorithm/`。 |

---

## 与历史 main 的行为差异

| 项 | main（旧） | 当前版本 |
|----|------------|----------|
| 调度完成 | dispatch 内 **fake-return** | 真实 AICore ACK/FIN（见 [Dispatch（任务下发 / 完成回收）](#dispatch任务下发--完成回收)） |
| Sim AICore | 无（仅 executor 可选） | **instant**（默认）或 72 pthread |
| 源码布局 | `esl_proxy/` 扁平 `src/` | `src/algorithm` + `src/platform` |
| QWEN3 默认 tier | 2（spmd-4） | **0**（non-spmd） |
| NODE_BUFF_SIZE | 8192（qwen3 编排易堆损坏） | **65536** |

---

## 已知限制

- **SPMD 调度不完整**：Qwen3 的 `QWEN3_SPMD_TIER` 仅在编排侧合并 task（`count > 1`、减少 task_cnt）；dispatch 仍对每个 task 只 publish 一次（`block_idx` 恒 0），AICore 也只执行一次 kernel，未按 block 扇出/逐 instance 完成。`executor.c` 中旧 SPMD 循环未接入线程。默认 tier=0（non-spmd）为当前测试基准。
- MIX 任务队列（`ready_queue[2]`）尚未实现，cutter 中 type 直映射 cube/vector 下标（dispatch 侧亦同，见 [Dispatch（任务下发 / 完成回收）](#dispatch任务下发--完成回收)）。
- `manager.c` 保留文件但未接入 sim/onboard 线程创建。
