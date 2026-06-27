# esl_proxy 实现文档

---

## 0. 概述

### 0.1 项目定位
`esl_proxy` 是 `simpler`（PTO Runtime 参考实现）的**精简代理实现**，用于在昇腾 a2a3 板上独立验证「DAG 调度 + 子任务派发 + AICPU↔AICore 握手」这条主链路，不依赖 simpler 的 Python/runtime。两种构建：

- **Sim**：纯 host CPU 模拟（pthread + 假 AICore），用于快速验证调度逻辑。
- **Onboard**：真实 NPU（AICPU 三线程 + AICore 24 block + CANN host launcher）。

两者共用同一套 `algorithm` 调度代码，仅平台后端不同（见第 3 章）。

### 0.2 整体架构

```
                 ┌───────────────── AICPU (3 线程) ─────────────────┐
 orchestration ─▶│  cutter(core0) ─▶ ready_queue ─▶ dispatch(core1)  │─▶ 寄存器 doorbell
   (case 产 DAG) │       ▲                               │           │
                 │       └──── completed_queue ◀─────────┘           │
                 └──────────────────────────────────────────────────┘
                                         │ EslDispatchPayload (GM, 512B 双缓冲/核)
                                         ▼
                          AICore  24 block (24 AIC + 48 AIV) 执行 kernel
                                         │ reg COND: ACK / FIN
                                         ▼
                          dispatch 轮询回收 ─▶ completed_queue ─▶ cutter 解依赖
```

- **三个 AICPU 角色线程**：`cutter`（解依赖、产 ready）、`dispatch`（下发/回收）、`orch`（跑 case 产 DAG）。索引 0/1/2，见 [esl_proxy/src/platform/onboard/aicpu_runtime.c:105-131](esl_proxy/src/platform/onboard/aicpu_runtime.c#L105-L131)。
- **AICore** 通过 `EslHandshake` 与 AICPU 握手拿到寄存器表，之后轮询寄存器 doorbell 取 `EslDispatchPayload` 执行。

### 0.3 与 simpler 的对应
| esl_proxy | simpler |
|---|---|
| `EslDispatchPayload`(512B) | `PTO2DispatchPayload` |
| `task_desc` | `PTO2TaskDescriptor` + `PTO2TaskPayload` 合并精简 |
| L2 swimlane | 同名 L2 swimlane（数据结构基本一致） |
| `cache_civac_range` | `simpler/.../aicpu/cache_ops.cpp` 的 `dc civac` |

---

## 1. 代码入口及运行方式

### 1.1 目录结构
```
esl_proxy/                      ← 仓库根 (git root)
├── esl_proxy/
│   ├── include/{algorithm,platform,swimlane}/
│   ├── src/
│   │   ├── algorithm/          ← 后端无关调度核心 (cutter/dispatch/ring_buf/...)
│   │   ├── platform/sim/       ← host 模拟后端
│   │   ├── platform/onboard/   ← 真实 NPU 后端 (CANN)
│   │   └── swimlane/           ← L2 泳道采集 (host + aicpu)
│   ├── cases/                  ← 编排用例 (paged_attention / qwen3)
│   └── Makefile                ← sim 构建
├── tools/run_onboard*.sh       ← onboard 构建+运行脚本
├── build/sources.sh            ← 共享源文件清单 (Makefile 与 run_onboard.sh 共用)
└── report/                     ← 本文档与 swimlane 产物
```

### 1.2 入口 `main.c`
单一入口、两种互斥构建模式，由 `ESL_PROXY_ONBOARD_HOST` 选择，见 [esl_proxy/src/main.c:8-23](esl_proxy/src/main.c#L8-L23)：

```c
#if ESL_PROXY_ONBOARD_HOST
extern int esl_onboard_run(int argc, char **argv);   // → host_onboard.c (CANN launcher)
int main(int argc, char **argv){ return esl_onboard_run(argc, argv); }
#else  /* sim：pthread 起 cutter/dispatch + 跑 case，最后比对 completed==task_id */
```

Sim 主流程：`esl_platform_init` → 起 `cutter_worker`/`dispatch_worker` 线程 → 跑 `aicpu_orchestration_entry`（case）→ join → `esl_platform_shutdown` → PASS/FAIL 判定，见 [esl_proxy/src/main.c:60-115](esl_proxy/src/main.c#L60-L115)。

### 1.3 Sim 构建与运行
```bash
cd esl_proxy/esl_proxy
make all            # 注意：默认目标是 asm，必须显式 all；产物 bin/esl_proxy
./bin/esl_proxy     # 默认 case=paged_attention_unroll_manual_scope
```
预期输出（PASS 判据：`g_completed_cnt == g_task_id`）：
```
[host] PASS: task_cnt=3096 subtask_cnt=3096
```

### 1.4 Onboard 构建与运行
三段构建：AICPU `.so`（含 algorithm + onboard）、AICore `.o`（ccec，AIC/AIV 两份）、host runner，见 [tools/run_onboard.sh](tools/run_onboard.sh)。

```bash
cd esl_proxy
export ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.0.0
bash tools/run_onboard_npu.sh        # task-submit --device auto 内部锁卡并构建+运行
```
预期：
```
esl_proxy onboard: task_cnt=1920 subtask_cnt=1920 completed_cnt=1920 ... OK
=== 任务完成 (exit=0) ===
```

### 1.5 构建/运行开关
| 变量 | 作用 | 默认 |
|---|---|---|
| `ORCH_CASE` | 选编排用例 `.h` | paged_attention_unroll_manual_scope.h |
| `QWEN3_SPMD_TIER` | SPMD 切分档 0..4（blocks_per_task=1/2/4/8/∞）；仅改变编排产出的 `count`，dispatch 当前不扇出（见 §2.4/附录C） | 0 |
| `ESL_PROXY_ENABLE_L2_SWIMLANE` | 构建期开泳道（链入 `swimlane_aicpu.c` + `-D...=1`） | 0 |
| `ESL_PROXY_L2_SWIMLANE_LEVEL` | 运行期泳道等级 0/1/2 | 0 |
| `WORKER_LOG` / `MAIN_LOG` | 编译进 worker/主日志 | 1 |

### 1.6 用例 `cases/`
`paged_attention_*` 与 `qwen3_*` 两族；AICPU 端通过 `#include cases/${ORCH_CASE}` 嵌入；perfetto 转换用对应的 `tools/*_func_names.json` 映射 kernel 名。

---

## 2. algorithm 修改的内容（相对 origin/base）

调度核心位于 [esl_proxy/src/algorithm/](esl_proxy/src/algorithm/) 与 [esl_proxy/include/algorithm/](esl_proxy/include/algorithm/)。相对 base 改动统计：12 文件 +542/-130，其中 `dispatch.c` 是大头（+389）。

### 2.1 任务 / 环形缓冲数据结构
按 task_id 索引的并行数组（`RING_SIZE=4096`），定义在 [esl_proxy/include/algorithm/ring_buf.h:31-39](esl_proxy/include/algorithm/ring_buf.h#L31-L39)，分配在 [esl_proxy/src/algorithm/shm.c:20-32](esl_proxy/src/algorithm/shm.c#L20-L32)：

| 数组 | 类型 | 角色 |
|---|---|---|
| `g_basic_buf` | `task_desc` | 任务描述（type/mode/count/duration/tensor 句柄/scalar） |
| `g_predecessors` | `predecessor_list` | 前驱列表（cnt + exp 指针，指向 `predecessor_storage` 环） |
| `g_predecessor_cnt` | `PredecessorCnt` | 每任务剩余前驱计数 |
| `g_successor_buf` | `node_list` | 后继列表（cutter 本地） |
| `g_state_buf` | `task_state` | 任务状态机（state/successor_cnt） |

**相对 base 的关键改动**：
- `conf.h`：`CON_NODE_CNT 32→256`、`NODE_BUFF_SIZE 8192→65536`、`AIC_CNT 60→ESL_PROXY_WORKER_BLOCK_DIM`（=24），`WORKER_LOG` 改为可被 `-D` 覆盖，见 [esl_proxy/include/algorithm/conf.h:6-30](esl_proxy/include/algorithm/conf.h#L6-L30)。
- `task.h`：修复 `TASK_TYPE_MIX 1→2`（曾与 VECTOR 同值导致 MIX 错乱）；`task_desc` 新增 `jitter_mask`；`predecessor_storage` 由 `malloc` 改为**静态数组**（板上无堆，见 [ring_buf.h:56](esl_proxy/include/algorithm/ring_buf.h#L56)）。
- `new_task()` 签名扩展为 `(task_id,type,count,duration_ns,jitter_mask)`，并补全 `type/id/jitter_mask/tensor_cnt/scalar_cnt`；新增 `advance_task_id()` 用 `wmb()+release` 发布 slot，见 [ring_buf.h:124-152](esl_proxy/include/algorithm/ring_buf.h#L124-L152)。

### 2.2 64B 对齐 + 缓存维护统一（本次改造）
非缓存一致的 AICPU 核之间、AICPU↔AICore 之间，跨核可见性全靠**显式 cache 维护**。本次把两个近乎重复的原语统一为一个 `dc civac`（clean+invalidate）原语，并把所有被维护的每任务结构体做 **64B 对齐**，从而**移除运行期对齐 rounding**。详见记忆 [[esl-proxy-cache-civac-unify]]。

- 单一原语 [esl_proxy/src/platform/onboard/cache_ops.c:20-29](esl_proxy/src/platform/onboard/cache_ops.c#L20-L29)：`dc civac` 循环 + `dsb sy`（去掉 `isb`，去掉 rounding；调用方保证 64B 对齐 + 64B 倍数）。旧名 `cache_invalidate_range`/`cache_flush_range` 作为宏别名保留。
- 结构体对齐 [esl_proxy/include/algorithm/task.h:52-87](esl_proxy/include/algorithm/task.h#L52-L87)：`task_desc`(448B)、`predecessor_list`(64B)、`node_list`(576B) 加 `__attribute__((aligned(64)))`；`g_predecessor_cnt` 由打包 `uint16_t[]` 改为 `PredecessorCnt{uint16_t v;}`（每计数独占一行，避免假共享）；附 `_Static_assert` 锁定尺寸。

### 2.3 Cutter（依赖解算）
入口 [esl_proxy/src/algorithm/cutter.c:171](esl_proxy/src/algorithm/cutter.c#L171) `cutter_worker`，主循环调 `deal_completed_queue`（[cutter.c:150](esl_proxy/src/algorithm/cutter.c#L150)），串起四步：
1. `update_task_state`（标完成 + 推进 `g_min_uncomplete_task`），[cutter.c:30](esl_proxy/src/algorithm/cutter.c#L30)。
2. `add_successors`（对新 commit 的任务，invalidate 三件套后解前驱，挂后继、写 `predecessor_cnt`，无前驱者入 ready），[cutter.c:55](esl_proxy/src/algorithm/cutter.c#L55)。
3. `resolve_dep`（完成任务回放其后继 `--predecessor_cnt`，归零则入 ready），[cutter.c:122](esl_proxy/src/algorithm/cutter.c#L122)。
4. `send_2_ready_queue`（batch_enqueue 到 dispatch 的 ready_queue），[cutter.c:110](esl_proxy/src/algorithm/cutter.c#L110)。

> MIX 队列尚未接入：当前仅 CUBE/VECTOR 入队（见 cutter.c 内 TODO）；`ready_queue_index` 把 MIX 归 CUBE/AIC。

### 2.4 Dispatch（下发 / 回收）—— 当前完整逻辑
[esl_proxy/src/algorithm/dispatch.c](esl_proxy/src/algorithm/dispatch.c) 是 algorithm 层改动最大的文件，承担「从 ready_queue 取任务 → 写 doorbell 下发给 AICore → 轮询回收 FIN → 入 completed_queue」整条流水。每个 dispatch 线程私有一套 `ctrl_t`（`free_bitmap`/`msg_bitmap`/`task_id_map1/2`）+ 全局 `g_executors[exe_type][core]` 槽位表。

**(1) 线程主循环** `dispatch_worker`（[dispatch.c:435](esl_proxy/src/algorithm/dispatch.c#L435)）：
```
while (!g_orch_is_done)            { dispatch(tid); dispatch_poll(tid); spin_wait(); }  // 编排进行中
while (g_completed_cnt < g_task_id){ dispatch(tid); dispatch_poll(tid); spin_wait(); }  // 排空收尾
g_is_done = true;  dispatch_publish_final_stats(elapsed);                                // 收官+发布统计
```

**(2) 一轮 `dispatch(tid)`**（[dispatch.c:392](esl_proxy/src/algorithm/dispatch.c#L392)）：`acquire` fence → `get_free_exe`（把上轮 `msg_bitmap` 完成位并回 `free_bitmap`，`set_mix` 合成 MIX 空闲位）→ `push_2_completed_queue` → 依次 `send_task(MIX)`、`send_task(VECTOR)`、`send_task(CUBE)`。

**(3) 下发 `send_task(ctrl, type)`**（[dispatch.c:353](esl_proxy/src/algorithm/dispatch.c#L353)）：
- 计算空闲核位图 `free = free_bitmap[slot0] & free_bitmap[slot1] & ((1<<AIC_CNT)-1)`，`cnt = popcount(free)`；为 0 则返回。
- `batch_dequeue` 从该类型 ready_queue 一次取 `cnt` 个任务；逐个用 `ctz(free)` 选最低空闲核、`slot 0` 优先否则 `slot 1`，调 `dispatch_submit`。
- 被拒（返回非 0）时把该任务单独 `batch_enqueue` 回退并 `break`。

**(4) 单任务下发 `dispatch_submit`**（[dispatch.c:263](esl_proxy/src/algorithm/dispatch.c#L263)）：
1. `cache_invalidate_range` 刷新 `g_basic_buf[slot]` / `g_predecessors[id]` / `g_predecessor_cnt[slot]`（跨核读最新任务数据）。
2. 占位本核槽：写 `g_executors[..].tasks[slot]=task_id`、`duration`、`task_id_map{1,2}[type][core]=task_id`，并 `free_bitmap[type][slot] &= ~mask` 标记忙。
3. `phys = esl_pick_phys_worker(core, exe_type)`（CUBE→偶 worker，VECTOR→奇 lane）。**若 `phys >= worker_count`** → 占位核，直接 `msg_bitmap |= 1<<core` 当场完成（无真实 AICore）。
4. 否则 `esl_prepare_subtask_to_core` 填 payload → `ESL_SWIMLANE_AICPU_ON_DISPATCH` → `wmb()` → `esl_publish_subtask_to_core` 写 doorbell（`write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, reg_task_id)`）→ `dispatch_mark_slot_complete` 立即探一次 FIN（sim 假核当拍即完成）。

**(5) payload 准备**（[dispatch.c:66/112](esl_proxy/src/algorithm/dispatch.c#L66)）：`esl_prepare_subtask_to_core` 取本核 GM `task` 基址 + 双缓冲槽 `reg_task_id & 1`，`build_payload` 写 `function_bin_addr`、`args[0]=duration`/`args[1]=jitter_mask`、`local_block_idx/num`、`args[48/49]` 指向 local/global context、`not_ready=0`。`reg_task_id` 由 `dispatch_next_reg_task_id` 每核单调自增（跳过 `AICORE_EXIT_SIGNAL`）。

**(6) 回收**：`dispatch_poll`（[dispatch.c:224](esl_proxy/src/algorithm/dispatch.c#L224)）遍历所有忙槽，读寄存器 `COND`，`platform_reg_task_finished` 命中则 `ack` 并置 `msg_bitmap`；`push_2_completed_queue`（[dispatch.c:340](esl_proxy/src/algorithm/dispatch.c#L340)）调 `dispatch_drain_completions`（[dispatch.c:324](esl_proxy/src/algorithm/dispatch.c#L324)）按 `task_id_map1/2` 把 `msg_bitmap` 解码成 `task_ids` → `batch_enqueue(completed_queue)` → `g_completed_cnt += n`（供 cutter `resolve_dep` 消费）。

**(7) 收尾统计** `dispatch_publish_final_stats`（[dispatch.c:406](esl_proxy/src/algorithm/dispatch.c#L406)）：扫 `g_state_buf` 统计未完成数/首个未完成，打包 task/subtask/completed/commit/n_uncomp/ready 计数，经 `platform_stats_publish` 写 device_wall 8×u64（host D2H 读出并判 PASS）。

> 注：当前**不做 SPMD 多 block 扇出**——`count=N` 的任务在 (4) 只下发一次（`block_idx` 恒 0），详见 §2.4 上文角标与附录 C。

### 2.5 Executor / ready queue / ctrl_t
- `executor_t`/`ctrl_t`：每 dispatch 线程私有的 `free_bitmap`/`msg_bitmap`/`task_id_map`，槽位空标记 `EXEC_SLOT_EMPTY=0xFFFF`（[esl_proxy/include/algorithm/executor.h:19](esl_proxy/include/algorithm/executor.h#L19)）。
- `queue.h` 的 `lock_q/unlock_q` 在 `unlock` 后补 `wmb()`（[esl_proxy/include/algorithm/queue.h:96-101](esl_proxy/include/algorithm/queue.h#L96-L101)），保证跨核可见。

### 2.6 `EslDispatchPayload` 与 `task_desc` 的关系
**两者是不同的数据结构，且应保持分离**（详见独立分析）：
- `task_desc`（[esl_proxy/include/algorithm/task.h:52](esl_proxy/include/algorithm/task.h#L52)）= 调度器侧、每任务（4096 ring slot）、可随意改。
- `EslDispatchPayload`（[esl_proxy/include/algorithm/runtime.h:45](esl_proxy/include/algorithm/runtime.h#L45)）= 设备 ABI、每核×2 双缓冲、512B 布局**冻结**（= simpler PTO2 ABI）。
- 桥接 `build_payload(out, desc, ...)` 每次派发现填。基数不同（per-task vs per-core）+ ABI 稳定性，决定二者**不能合并**。

### 2.7 非一致核可见性策略
AICPU 三核互不缓存一致：**单写者变量无需原子**，跨核可见靠 cache flush/invalidate。本次保留 QuteMiao 的原子选择（见 [[esl-proxy-preserve-qutemiao-atomics]]）：`g_predecessor_cnt`（cutter 单写）等可去原子；`g_orch_is_done` 必须 `volatile`（spin 循环防编译器提升）。

---

## 3. platform 平台设计

> 该层（[esl_proxy/src/platform/](esl_proxy/src/platform/)、[esl_proxy/include/platform/](esl_proxy/include/platform/)）相对 base **基本为新增**。

### 3.1 统一 HAL 接口
[esl_proxy/include/platform/platform.h](esl_proxy/include/platform/platform.h) + [platform_regs.h](esl_proxy/include/platform/platform_regs.h) 定义同一组接口，sim 与 onboard 各自实现：`esl_platform_init/shutdown`、`read_reg/write_reg`、`cache_civac_range`、`platform_stats_publish`、`platform_pick_phys_worker`、trace 等。算法层只 include `platform.h`，对后端无感。

### 3.2 Sim 后端
[esl_proxy/src/platform/sim/](esl_proxy/src/platform/sim/)：`platform_sim.c`（init/shutdown）、`sim_core_regs.c`（寄存器模拟）、`device_runner.c` + `aicore_wrapper.cpp`（假 AICore：在 host 线程里跑 `aicore_execute` 并回写 FIN）、`cache_ops.c`（no-op + 编译屏障）、`onboard_trace_sim.c`（trace 落 stdout）。用于无设备验证调度逻辑。

### 3.3 Onboard 后端
[esl_proxy/src/platform/onboard/](esl_proxy/src/platform/onboard/)。

**3.3.1 AICPU 运行时** — [aicpu_runtime.c](esl_proxy/src/platform/onboard/aicpu_runtime.c)：CANN kernel 入口 `esl_aicpu_init`/`esl_aicpu_exec`；`esl_aicpu_execute`（[aicpu_runtime.c:68](esl_proxy/src/platform/onboard/aicpu_runtime.c#L68)）按线程 idx 分派 cutter/dispatch/orch，含 `init_once` 单次初始化、worker barrier、finished barrier 与最终 `esl_platform_shutdown`。`aicpu_dispatcher.c`/`platform_init.c` 提供加载与平台初始化。

**3.3.2 AICore 入口** — [aicore_entry.cpp](esl_proxy/src/platform/onboard/aicore_entry.cpp)（ccec）：`KERNEL_ENTRY(aicore_kernel)` 按 AIC/AIV 算 `block_idx`、设 ffts base、初始化泳道 head slot，再调算法层 `aicore_execute`（[esl_proxy/src/algorithm/aicore_executor.c:24](esl_proxy/src/algorithm/aicore_executor.c#L24)）：握手→轮询寄存器→取 payload→`fake_kernel_run`→回写 ACK/FIN。

**3.3.3 Host runner** — [host_onboard.c](esl_proxy/src/platform/onboard/host_onboard.c)：`esl_onboard_run`（[host_onboard.c:758](esl_proxy/src/platform/onboard/host_onboard.c#L758)）做 H2D（dispatcher.so/inner.so/k_args/AICore regs）、分配 device_wall、并按**关键 launch 顺序**启动：先 `aicpu init`，再 **AICore（独立 `stream_aicore`，先于 exec，使核常驻以待握手）**，最后 `aicpu exec`（[host_onboard.c:1011-1062](esl_proxy/src/platform/onboard/host_onboard.c#L1011-L1062)）；同步后 D2H dump device_wall。AICore 与 AICPU exec **必须不同 stream**，否则同序流死锁。

**3.3.4 寄存器 / HAL** — [npu_hal.c](esl_proxy/src/platform/onboard/npu_hal.c) 实现真实 MMIO `read_reg/write_reg`；RegId 仅 `DATA_MAIN_BASE/COND/FAST_PATH_ENABLE`（[platform_config.h](esl_proxy/include/platform/platform_config.h)）。

**3.3.5 握手协议** — [handshake.c](esl_proxy/src/algorithm/handshake.c) ↔ aicore_executor 四态机：
`aicpu_ready`(AICPU置) → `aicore_regs_ready`(AICore置, 报 physical_core_id) → `aicpu_regs_ready`(AICPU填寄存器表后置) → `aicore_done`(AICore置=i+1)。
每步用 `OUT_OF_ORDER_STORE_BARRIER` + `cache_flush_range(wk, sizeof(*wk))` 发布整行 `EslHandshake`（64B 对齐单行）。

**3.3.6 跨核同步与屏障** — [cache_ops.c](esl_proxy/src/platform/onboard/cache_ops.c)（civac 原语）、[memory_barrier.h](esl_proxy/include/platform/memory_barrier.h)（`ESL_RMB/WMB=dsb ld/st`、`OUT_OF_ORDER_*_BARRIER=dmb ish*`、`rmb/wmb` 别名）、[platform_sync_onboard.c](esl_proxy/src/platform/onboard/platform_sync_onboard.c)。


### 3.4 并发模型
- **72 worker = 24 AIC + 48 AIV**，`block_dim=24`，AIV 每 block 2 lane。
- **AICPU 3 线程**：cutter=core0 / dispatch=core1 / orch=core2（`_Static_assert` 锁 `ESL_PROXY_AICPU_THREAD_NUM=3`）。
- `platform_pick_phys_worker(core, exe_type) = core*EXE_TYPE_CNT + exe_type`：CUBE→偶、VECTOR→奇 worker，`phys>=worker_count` 快速完成。

---

## 4. 泳道图（L2 swimlane）实现

> 全模块新增；构建期 `ESL_PROXY_ENABLE_L2_SWIMLANE=1` 才链入，运行期 `ESL_PROXY_L2_SWIMLANE_LEVEL` 控制。

### 4.1 目的与数据模型
采集每个 AICore 任务的执行时间，导出 perfetto 泳道图。等级：0=关、1/2=AICore timing（[swimlane_aicpu.c:106-110](esl_proxy/src/swimlane/swimlane_aicpu.c#L106-L110) 把非 0 收敛到 timing）。核心记录 `L2SwimlaneAicoreTaskRecord = {start_time, end_time, task_token_raw, reg_task_id}`（[esl_proxy/include/swimlane/swimlane_types.h:72-77](esl_proxy/include/swimlane/swimlane_types.h#L72-L77)），@50MHz。

数据布局（GM）：每核一个 `L2SwimlaneAicoreTaskPool` = `ActiveHead(64B)` + `FreeQueue(128B)`；AICore 写满一个 `TaskBuffer` 后由 AICPU 经 free_queue 轮换缓冲（[swimlane_types.h:148-175](esl_proxy/include/swimlane/swimlane_types.h#L148-L175)）。

### 4.2 设备侧采集（AICore）
[esl_proxy/include/swimlane/swimlane_aicore.h](esl_proxy/include/swimlane/swimlane_aicore.h)：
- `KERNEL_ENTRY` 时 `ESL_SWIMLANE_AICORE_KERNEL_ENTRY` 从 `rotation_table[block_idx]` 取本核 head slot（[swimlane_aicore.h:68-79](esl_proxy/include/swimlane/swimlane_aicore.h#L68-L79)）。
- 每个任务 `TASK_BEGIN`(记 start) → 执行 → `TASK_RECORD` 调 `l2_swimlane_aicore_record_task`（[swimlane_aicore.h:27-62](esl_proxy/include/swimlane/swimlane_aicore.h#L27-L62)）：`dcci(head)` 读当前缓冲，写 record，`dcci(record, CACHELINE_OUT)+dsb` 刷出。在 `aicore_execute` 主循环里调用（[esl_proxy/src/algorithm/aicore_executor.c:84-86](esl_proxy/src/algorithm/aicore_executor.c#L84-L86)）。

### 4.3 AICPU 侧
[esl_proxy/src/swimlane/swimlane_aicpu.c](esl_proxy/src/swimlane/swimlane_aicpu.c)：`l2_swimlane_aicpu_init`（[swimlane_aicpu.c:94](esl_proxy/src/swimlane/swimlane_aicpu.c#L94)）按 worker 填 `head_table[i]=&pool.head`、prime 首个缓冲，最后 `cache_civac_range(head_table, 向上取整 64B)`（[swimlane_aicpu.c:149-153](esl_proxy/src/swimlane/swimlane_aicpu.c#L149-L153)，本次 cache 改造涉及）。运行中 `aicore_rotate` 在缓冲写满时从 free_queue 取新缓冲。`ESL_SWIMLANE_AICPU_SET_ORCH_THREAD` 标记 orch 线程。

### 4.4 Host 侧导出
[esl_proxy/src/swimlane/host_swimlane.c](esl_proxy/src/swimlane/host_swimlane.c)：运行后 D2H 读各核缓冲，导出 `l2_swimlane_records.json`（raw，顶层 `aicore_tasks` 数组，行 = `[core, token, reg_task, start, end]`），再经 `tools/swimlane_to_perfetto.py` + `*_func_names.json` 转 `l2_swimlane_trace.json`（perfetto）。

### 4.5 构建/运行开关与产物
```bash
cd esl_proxy
export ESL_PROXY_ENABLE_L2_SWIMLANE=1 ESL_PROXY_L2_SWIMLANE_LEVEL=2
bash tools/run_onboard_npu.sh          # 或 run_onboard_swimlane_cases.sh 跑全 case
```
产物：仓库根 `l2_swimlane_records.json` / `l2_swimlane_trace.json`；批量模式落 `report/swimlane/<case>/`。

### 4.6 验证结果
板上实测（paged_attention，level 2）：`1920/1920 OK`，`l2_swimlane_records.json` 含 **1920 条 aicore_tasks**，perfetto trace ~824KB；各 kernel 时延与配置的 fake-kernel duration 吻合（见 [[esl-proxy-onboard-72worker-run]]）。
