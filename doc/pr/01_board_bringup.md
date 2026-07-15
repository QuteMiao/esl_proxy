# PR-A：上板 bring-up（basic + fake kernel）

相对基线：`pr/A0-layout`（目录搬迁见 `doc/pr/00_layout.md`）。

本 PR 在 NPU 上用 **fake kernel** 跑通 **paged attention**（`task/subtask/completed = 1920`）。

---

## 1. 为什么修改

`main` 是 **Fake Return** 骨架：

- dispatch 不写门铃、不读 COND；
- 无 AICPU↔AICore 握手与 per-core payload；
- 无 Host / AICPU / AICore 三端构建与 runner；
- 算法与平台能力挤在平坦目录，无法按 onboard 后端分流。

要在真机上验证「编排 → 下发 → 核侧执行 → 完成回收」，本 PR 补齐 **platform 分层 + 真实调度闭环 + onboard 三端链路**（basic + fake kernel）。

---

## 2. 怎么修改

### 2.1 目录与构建

| 动作 | 路径 |
|------|------|
| 新增 | `include/platform/`、`include/platform/a2a3/`、`include/platform/tools/`、`src/platform/a2a3/` |
| 新增 | `cmake/{aicpu,aicore,host}`、`cmake/sources.cmake`、`tools/run_onboard.sh` |
| 调整 | `Makefile`：增加 platform include |

三端职责：

| 端 | 构建 | 职责 |
|----|------|------|
| Host | `cmake/host` → `esl_onboard_runner` | ACL 分配 Runtime/Payload/KernelArgs，先 AICore 后 AICPU，读 stats |
| AICPU | `cmake/aicpu` → `.so` | 握手、orch/cutter/dispatch、prepare/`wmb`/门铃 |
| AICore | `cmake/aicore` → `.o` | 握手对端 → 读门铃 → ACK → `fake_kernel_run` → FIN |

```
orch → cutter → ready_queue
              ↓
dispatch: poll COND / prepare+wmb+doorbell
              ↓
AICore: ACK → fake_kernel_run → FIN → Host stats OK
```

### 2.2 Algorithm：新增

| 文件 | 改了什么 | 为什么 |
|------|----------|--------|
| `runtime.h` | `EslHandshake` / `EslRuntime` / 512B `EslDispatchPayload` | Host/AICPU/AICore 统一布局 |
| `handshake.{h,c}` | ready → regs_ready → 缓存 reg/cond → done；shutdown EXIT | 调度前确认物理核与 COND |
| `dispatch_a3.c`（含原 payload prepare/publish） | `reg_task_id`、prepare/publish 分离、`esl_init_global_context` | 门铃与 payload 槽一一对应；批量 prepare 后一次 `wmb` |
| `fake_kernel.h` | `fake_kernel_run(duration_ns, jitter_mask)` | 先验调度闭环，不引入真实算子 |
| `aicore_executor.{h,c}` | `aicore_execute`：门铃 → ACK → fake → FIN | 核侧状态机；CANN 入口在 platform |

### 2.3 Algorithm：手术补丁

| 文件 | 改了什么 | 为什么 |
|------|----------|--------|
| `conf.h` | `AIC_CNT` 跟拓扑；`CON_NODE_CNT` 32→256；`NODE_BUFF_SIZE` 8192→65536 | 对齐 24AIC；PA 图需要更大缓冲 |
| `task.h` | `TASK_TYPE_MIX=2`；`duration`→`uint32_t` ns；加 `jitter_mask` | 与 VECTOR 解冲突；假核按时长忙等 |
| `dispatch.{h,c}` | Fake Return → 读 COND + prepare/publish；只发 CUBE/VECTOR、`block_idx=0`；新增 `g_runtime`、`dispatch_poll` | 真机完成闭环 |
| `cutter.{h,c}` | `g_state_buf` → BSS `state_storage` | AICPU 避免堆 |
| `ring_buf.h` / `shm.c` | 前驱/state 改 BSS；新增 `advance_task_id` | 上板避免堆；提交可见性与编号分离 |
| `executor.*` / `queue.h` | `EXEC_SLOT_EMPTY`；`unlock_q` 后 `wmb` | 空槽与 task_id 0 区分；跨可见域屏障 |

### 2.4 Platform（整层新增）

| 区域 | 主要文件 | 作用 |
|------|----------|------|
| 抽象 | `platform.h`、`platform_config.h`、`platform_regs.h`、`worker_map.h`、`memory_barrier.h` | 24AIC+48AIV、寄存器、ACK/FIN、`wmb`/`rmb` |
| Onboard HAL | `npu_hal.c`、`tools.c`、`cache_ops.c` | 寄存器、加载 SO、刷 cache |
| Host / AICPU / AICore | `host_onboard.c`、`aicpu_*.c`、`aicore_entry.cpp` | 拉起、握手后起调度、CANN 入口 |

---

## 3. 与 simpler 的对应

对照以 **a2a3** 为准（a5 路径镜像）。

| esl_proxy（本 PR） | simpler（a2a3） |
|--------------------|-----------------|
| `algorithm/` vs `platform/` | `runtime/` vs `platform/{onboard,sim}/` + `src/common/platform/` |
| Host：ACL + 先 AICore 后 AICPU | `platform/onboard/host/device_runner.cpp`（`launch_aicore` → `launch_aicpu`）；基类 `src/common/platform/onboard/host/device_runner_base.*` |
| `EslHandshake` / `esl_handshake_*` | `Handshake`：`…/runtime/runtime.h`；AICPU `scheduler_cold_path.cpp`；AICore `aicore_executor.cpp` |
| 512B payload + prepare/publish + `DATA_MAIN_BASE` / COND ACK-FIN | `PTO2DispatchPayload`（`pto2_dispatch_payload.h`）；`scheduler_dispatch.cpp` / `scheduler_completion.cpp`；AICore `MAKE_ACK/FIN_VALUE` |
| `fake_kernel_run` | simpler 走真实 `function_bin_addr` → `execute_task`；本 PR 用忙等代替算子 |
| cutter/dispatch 取 ready 任务 | simpler：shape ready 队列 + `get_ready_task` |
| PA case | `examples/a2a3/.../paged_attention*`、`tests/st/a2a3/**/paged_attention*` |
