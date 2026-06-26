# AIC Kernel 计算时间估算（Qwen3 / 昇腾 A3）

> 路径：`V200-benchmark/qwen3/qwen3_dynamic_manual_scope/kernels/aic/`
> 日期：2026-06-04　模型：Qwen3-14B（decode 阶段）　精度：BF16 输入 / FP32 累加

本文用 **Roofline 模型 + 泳道图实测校准**，估算昇腾 A3（单 die）上 qwen3 decode「执行一次核函数」的时间，并判断瓶颈在算力还是带宽。

---

## 0. 计算逻辑总览（先读这里）

**输入有两组参数**：① **kernel 形状**（M,K,N,blocks,dtype，来自源码，见 §1）；② **硬件参数**（算力/带宽/各级容量与速率，每个标数据来源，见 §2）。两组参数代入下面的公式即得时间。

**核心公式（每次核函数发射）**

```
FLOPs   = 2·M·K·N                                  ← 由 kernel 形状(§1)
Bytes   = 读字节(激活+权重) + 写字节(输出)          ← 读写合一，由 kernel 形状(§1)
per_core, cores, BW                                ← 由硬件参数(§2)

口径 A 从算力:  t_A = FLOPs / (cores × per_core)
口径 B 从带宽:  t_B = Bytes / BW                    （BW=1.6TB/s，读写共享、全 die 共享）
口径 C 合并  :  t_C = t_A + t_B                     （无重叠上界；实际 ∈ [max(A,B), A+B]）

判瓶颈:  算术强度 AI = FLOPs/Bytes  vs  脊点 ridge = P/BW(≈237)
         AI < ridge → 带宽瓶颈(decode 恒此) ; AI > ridge → 算力瓶颈
```

**五步流程**：① 取 kernel 形状(§1) → ② 取硬件参数+来源(§2) → ③ 三口径算时间(§3) → ④ 代入数值并与实测对比(§4/§5/§6) → ⑤ 判瓶颈、扩展到 paged attention 与多级存储(§7/§8/§9)。

**物理数据流向**：`HBM(=GM) ⇄ L2(透明缓存,全die共享) ⇄ L1/UB(每核私有) ⇄ L0 ⇄ Cube`。`t_B` 只对**真正下到 HBM** 的字节用 BW；留在 L2 的中间量走更快的 L2 速率（§9）。

**章节地图**：§1 kernel 参数 · §2 硬件参数+来源 · §3 公式(三口径) · §4 代入 · §5/§6 实测对比 · §7 算力 vs 带宽瓶颈 · §8 paged attention · §9 单层 HBM/L2 多级时间。

---

## 1. 从源码提取的 kernel 参数

所有 aic kernel 都是 `[M×K] × [K×N] → [M×N]` 的 GEMM，`M = batch_padded = 16`（decode 批大小），输入/权重 BF16，累加 FP32。下表数值来自源码常量与 Tile/GlobalTensor shape：

| kernel | 含义 | M | K | N | K-tile循环 | N/block | block数 |
|---|---|---:|---:|---:|---|---:|---:|
| `q_proj` | Q 投影 | 16 | 5120 | 5120 | 40×128 | 256 | 20 |
| `k_proj` | K 投影 | 16 | 5120 | 1024 | 40×128 | 128 | 8 |
| `v_proj` | V 投影 | 16 | 5120 | 1024 | 40×128 | 128 | 8 |
| `out_proj_residual` | O 投影 | 16 | 5120 | 5120 | 40×128 | 128 | 40 |
| `gate_proj` | FFN gate | 16 | 5120 | 17408 | 40×128 | 512 | 34 |
| `up_proj` | FFN up | 16 | 5120 | 17408 | 40×128 | 512 | 34 |
| `down_proj` | FFN down | 16 | 17408 | 5120 | 136×128 | 128 | 40 |
| `qk_matmul` | Q·Kᵀ 注意力分数 | 16 | 128(head_dim) | S | 动态(KV块) | 128 | — |
| `sv_matmul` | Score·V | 16 | S | 128(head_dim) | 动态(KV块) | 128 | — |

来源对应关系（以 `q_proj.cpp` 为例）：`v16=5120`（K=hidden）、循环 `v12=40` 步长 `v11=2` → 40 个 128 的 K-tile = 5120；`v14=256`=每 block 的 N，输出 stride 5120 → 全 N=5120。其余 kernel 同理：
- hidden = **5120**，FFN intermediate = **17408**（`v14/v13=17408`），KV 投影 N = **1024**（8 KV heads × 128）。
- 注意力 `qk/sv`：head_dim **128**，GQA `H_q=40`、`H_kv=8`，KV 分页块大小 8（`v13=8`），循环上界为动态 KV 长度，故按序列长度 **S** 参数化。

模型常量：`hidden=5120, intermediate=17408, q_heads=40, kv_heads=8, head_dim=128, layers=40`。

---

## 2. 硬件参数与数据来源

### 2.1 总表（每个参数的数值 + 来源）

| 参数 | 数值 | 数据来源 |
|---|---|---|
| 芯片 | Ascend910（A3 / 910C 级，双 die 封装） | **npu-smi** |
| 每卡 die 数 | 2 | **npu-smi** `Chip Count` |
| Cube(AIC) 核数 | **24 可用**（25 物理） | **ini** `ai_core_cnt=24` / **npu-smi** `Aicore Count=25` |
| Vector(AIV) 核数 | 48 | **ini** `vector_core_cnt=48`；trace AIV_24..71 |
| Cube 频率 | **1850 MHz** | **npu-smi** `Aicore Freq` / **ini** `cube_freq=1850` |
| Cube MAC 阵列 | 16×16×16 | **ini** `cube_m/n/k_size=16` |
| 单核 BF16 峰值 `per_core` | **15.155 TFLOP/s** | **推导** = 16³×2×1.85e9（见 §2.2） |
| die BF16 峰值 `P` | **378.9 TFLOP/s**（25 核物理上界；脊点用此） | **推导** = 25×`per_core`；`t_A` 按 **24 核**算 |
| **HBM 带宽 `BW`** | **~1.6 TB/s**（读+写共享、全 die AIC+AIV+DMA 共享） | **公开参考**(910C)；**ini** `ddr_rate=32`B/cyc×24×1.85≈1.42 TB/s 同量级 |
| HBM(=GM) 容量/die | 64 GB | **npu-smi** `HBM Capacity=65536MB` / **ini** `memory_size` |
| **L2 容量** | **192 MB**（全 die 共享） | **ini** `l2_size=201326592` |
| **L2 读 / 写速率** | **110 / 86 B/cyc/核** → 聚合 **4.88 / 3.82 TB/s** | **ini** `l2_read_rate=110` / `l2_write_rate=86` |
| L1 Buffer | 512 KB / 核（私有） | **ini** `l1_size=524288` |
| L0A / L0B / L0C | 64 / 64 / 128 KB（私有） | **ini** `l0_a/b/c_size` |
| UB（向量） | 192 KB / 核（私有）；UB→L2 64 B/cyc | **ini** `ub_size=196608` / `ub_to_l2_rate=64` |
| L1→L0A 速率 | 512 B/cyc | **ini** `l1_to_l0_a_rate=512` |

> **来源说明**：
> - **npu-smi** = 本机 `task-submit --device auto` 跑 `npu-smi info`（device 4，`npu-smi 26.0.rc1`，Board `0x70`）实测；
> - **ini** = 本机 CANN 平台配置 `/usr/local/Ascend/cann-9.0.0/aarch64-linux/data/platform_config/Ascend910B1.ini`（与本机吻合：`ai_core_cnt=24`、`cube_freq=1850`、`memory_size=64GB`、`l2_size=192MB`）；速率取自其 `[AICoreMemoryRates]` 段（B/cycle/核）；
> - **公开参考** = 910C 公开资料（见文末来源）；**推导** = 由上述实测/配置按公式算。
> 本文一律以**单 die** 为基准（实际运行 `thread4_block24` = 24 个 AIC 核）；整卡双 die（P=757.8 TFLOP/s、BW≈3.2 TB/s、128 GB）结果在末尾给出。

### 2.2 关键推导：BF16 峰值算力

Da Vinci Cube 为 16×16×16 MAC 阵列，每 cycle 16³=4096 MAC = 8192 FLOP：

```
per_core = 16×16×16 × 2 × 1.85e9 = 15.155 TFLOP/s          （单核 BF16）
P_die    = 24(可用核) × 15.155 = 363.7 TFLOP/s             （实际并行基准）
         = 25(物理核) × 15.155 = 378.9 TFLOP/s             （物理上界，脊点 ridge=P/BW≈237 用此）
脊点 ridge = P/BW = 378.9 / 1.6 ≈ 237 FLOP/B
```

> `t_A` 用 `min(blocks,24)×per_core`（按可用 24 核），脊点用物理上界 378.9——两处口径差异已在各节标注，不影响「带宽瓶颈」结论。

### 2.3 数据流向与存储层级（物理）

`HBM(=GM) ⇄ L2(透明缓存,全die共享192MB) ⇄ L1/UB(每核私有) ⇄ L0 ⇄ Cube`

- **HBM 与 GM 同物**：GM（Global Memory）是编程模型里 kernel 看到的全局地址空间（`__gm__`/`GlobalTensor`），HBM 是其物理介质；`TLOAD` 一个 GM 地址物理上即从 HBM 取（过 L2）。
- **私有 vs 共享**：L0/L1/UB 每核私有（核内独享、带宽极高，不计入 1.6 TB/s）；L2、HBM 全 die 共享。

### 2.4 L2 会频繁失效吗？——对大流式数据：是（且无妨）

是否命中 L2 取决于**有无复用**与**数据是否 ≤ 192 MB**：

- **权重 / KV（流式、读一次、远大于 L2）→ L2 基本失效**：单个 gate 权重 178 MB 就快占满 192 MB L2，40 层权重 ~26 GB、paged KV ~4 GB 都 ≫ L2；这些数据每字节只读一次、无复用，下一层/下一块立即把上一份挤掉 → 命中率极低，近似旁路。**但这无妨**——本就没有复用可缓存，正是「数据只读一次、缓存帮不上」让 decode 牢牢带宽受限，故 `t_B` 直接按 HBM 带宽算是对的。
- **小而复用的数据 → 命中率高**：广播激活（q_proj 0.164 MB）、qi 等被多个 block 重复读，可常驻 L2——这正是「`A_in` 一阶上界、L2 吸收部分重复读」的依据。
- **paged attention 更糟**：KV 块按 `block_table` 随机分散、读一次，L2 几乎全 miss → 即 §8.4「离散访存致有效带宽远低于峰值」的微观原因。

---
## 3. 计算公式（三种口径）

每个 aic kernel 都是分块矩阵乘 `C[M,N] = A[M,K] × B[K,N]`，输出按列切成 `blocks` 个 SPMD block，分布到 `cores = min(blocks, 24)` 个 AI Core 上并行。三种口径如下。

### 3.1 口径 A —— 从算力出发

**思路**：只看 Cube 算力、假设数据供给无限快。FLOPs 由 GEMM 形状决定，时间 = FLOPs ÷（占用核数 × 单核峰值）。

**公式**

```
FLOPs    = 2 × M × K × N                          （每个输出元素 K 次乘加，乘+加=2）
per_core = 16×16×16 × 2 × 1.85e9 = 15.155 TFLOP/s （单核 BF16 峰值，来源 §2.2）
cores    = min(blocks, 24)                         （本次发射占用核数，24 为可用上限）
t_A      = FLOPs / (cores × per_core)
```

**(表 1) 各 aic kernel 的 `A × B = C` 是什么、shape 多少**——A=左矩阵（激活/Q/P），B=右矩阵（权重/Kᵀ/V），C=输出；A、B 为 BF16，C 为 FP32。每个 cell 写「参数名 `[shape]`」：

| kernel | A（左，`[M×K]`） | B（右，`[K×N]`） | C（输出，`[M×N]`） | blocks |
|---|---|---|---|---:|
| q_proj | `normed_tile` `[16×5120]` | `wq` 权重 `[5120×5120]` | `q_proj_out` `[16×5120]` | 20 |
| k_proj | `normed_tile` `[16×5120]` | `wk` 权重 `[5120×1024]` | `k_proj_out` `[16×1024]` | 8 |
| v_proj | `normed_tile` `[16×5120]` | `wv` 权重 `[5120×1024]` | `v_proj_out` `[16×1024]` | 8 |
| o_proj | `attn_out` `[16×5120]` | `wo` 权重 `[5120×5120]` | `hidden_states` `[16×5120]` | 40 |
| gate_proj | `post_norm_tile` `[16×5120]` | `w_gate` 权重 `[5120×17408]` | `gate_tile` `[16×17408]` | 34 |
| up_proj | `post_norm_tile` `[16×5120]` | `w_up` 权重 `[5120×17408]` | `up_tile` `[16×17408]` | 34 |
| down_proj | `mlp_tile` `[16×17408]` | `w_down` 权重 `[17408×5120]` | `down_tile` `[16×5120]` | 40 |
| qk_matmul\* | `all_q_padded`（Q）`[16×128]` | `k_cache`（Kᵀ）`[128×S]` | `all_raw_scores`（分数）`[16×S]` | 20 |
| sv_matmul\* | `all_exp_padded`（P）`[16×S]` | `v_cache`（V）`[S×128]` | `all_oi_tmp`（输出）`[16×128]` | 20 |

> M=16=`batch_padded`，K/N 见上；7 个投影 `FLOPs = 2·M·K·N`。
> \* 注意力为**每头**一个 `[16×128]×[128×S]=[16×S]`（qk）/ `[16×S]×[S×128]=[16×128]`（sv），上表 shape 为单头；按 **40 个 q 头**合计 `FLOPs = 2·B·H_q·S·d`（B=16, H_q=40, d=128, S=4096）。KV 的 B/H_kv=8（GQA，q 头共享）。

**(表 2) 算力角度的计算时间 `t_A`**（per_core = 15.155 TFLOP/s = 1.5155e13）：

| kernel | FLOPs = 2·M·K·N | cores=min(blocks,24) | t_A = FLOPs/(cores×per_core) |
|---|---|---:|---:|
| q_proj | 2×16×5120×5120 = 0.839 G | 20 | 0.839e9/(20×1.5155e13) = **2.77 µs** |
| k_proj | 2×16×5120×1024 = 0.168 G | 8 | 0.168e9/(8×1.5155e13) = **1.38 µs** |
| v_proj | 0.168 G | 8 | **1.38 µs** |
| o_proj | 2×16×5120×5120 = 0.839 G | 24 | 0.839e9/(24×1.5155e13) = **2.31 µs** |
| gate_proj | 2×16×5120×17408 = 2.852 G | 24 | 2.852e9/(24×1.5155e13) = **7.84 µs** |
| up_proj | 2.852 G | 24 | **7.84 µs** |
| down_proj | 2×16×17408×5120 = 2.852 G | 24 | **7.84 µs** |
| qk_matmul | 2×16×40×4096×128 = 0.671 G | 20 | 0.671e9/(20×1.5155e13) = **2.21 µs** |
| sv_matmul | 0.671 G | 20 | **2.21 µs** |
| **7 proj 合计** | **10.57 G** | — | **31.4 µs** |

**怎么算的（以 gate_proj 为例）**：① FLOPs = 2×16×5120×17408 = 2.852e9；② blocks=34 > 24 → cores=24；③ t_A = 2.852e9 / (24 × 1.5155e13) = **7.84 µs**。

> 算力侧整层只需 ~31 µs，极小——因 decode 的 M=16 太小、权重复用低（详见 §7）。**实际时间由带宽决定（口径 B）**，t_A 只作下界。

### 3.2 口径 B —— 从带宽出发（详细数据搬运模型）

带宽口径要把**每一笔从 HBM 经 DMA 搬运的数据**都数出来。一次 matmul 在 Da Vinci 上的数据通路：

```
HBM ──(MTE2/DMA)──► L1 ──(MTE1)──► L0A/L0B ──(Cube)──► L0C ──(FIX)──► HBM
       读 A、B 权重                       矩阵乘累加        写 C
```

对 HBM 带宽有贡献的只有与 HBM 直接交互的两端：**读入 A、B** 与 **写出 C**。逐笔列出：

**(a) 读入激活 A（GM→L1）。** 每个 block 为完成自己那 `Nb = N/blocks` 列的输出，需要沿 K 方向加载**整列**激活 `A[M,K]`。源码中即循环里反复 `TLOAD` 的 `[M,128]` 输入分块，K 方向累计 `K/128` 次，单 block 合计 `M×K`。由于 `blocks` 个 block 各跑在不同核上、各自从 GM 读一份 A：

```
Bytes_A = blocks × M × K × din          （din = 2 字节，BF16）
```

**(b) 读入权重 B（GM→L1）。** 权重按列被切给各 block，互不重叠，全部 block 合起来恰好把 `B[K,N]` 读一遍：

```
Bytes_B = Σ_block (K × Nb × dw) = K × N × dw      （dw = 2 字节，BF16）
```

**(c) 写出结果 C（L0C→GM，FIX 流水）。** 最终 `TSTORE` 把累加器写回 HBM，FP32：

```
Bytes_C = M × N × dout                   （dout = 4 字节，FP32）
```

**单次核函数的总 HBM 流量与时间：**

```
Bytes_total = Bytes_A + Bytes_B + Bytes_C        ← 读字节(A,B) 与 写字节(C) 相加
            = blocks·M·K·din + K·N·dw + M·N·dout
t_B = Bytes_total / BW                    （BW = 1.6 TB/s）
```

> **关于 1.6 TB/s 的口径**：这是**整个 die 的 HBM 聚合带宽，读+写共享一个数**，不是"读 1.6 + 写 1.6"。HBM 数据总线双向时分复用，同一时刻只能读或写，故 roofline 把**读字节与写字节加在一起再除以 1.6 TB/s**（上式正是如此）。该带宽由全 die 所有单元共享——**AIC + AIV + DMA 同抢**（见 §8.5 paged attention 聚合带宽）。decode 下读(权重/KV)占 ~99%、写(FP32 输出)占比极小，即便读写分开也几乎不变。
>
> 数据通路（软件可见搬运 + L2 透明缓存）：`HBM ─MTE2/TLOAD→ L1 ─MTE1/TMOV→ L0A/L0B ─Cube/TMATMUL→ L0C ─FIX/TSTORE→ HBM`；L2 是 GM↔L1 路上的透明缓存（命中则不下 HBM）。**L1/L0/UB 每核私有**，只有 L2、HBM 跨核共享——故 `t_B` 只对真正下到 HBM 的字节用 1.6 TB/s，核内流式小 tile 与 L2 命中不重复计。

> 注：A 被各 block 重复读，是一阶上界（实际 L2 cache 可吸收一部分重复读）。但权重项 `K·N·dw` 占 88–96%，是绝对主导，A、C 仅作修正。

注意力 kernel 同理，把权重换成 KV cache：`Bytes = blocks·M·d·din(读Q) + 2·S·H_kv·d·dw(读K、V) + M·H_q·S·dout(写分数)`。

### 3.3 口径 C —— 算力与带宽同时考虑

口径 A、B 分别假设另一方无限快。真实 kernel 两者都要花时间，二者**无法完全重叠**（搬入首块才能开算、算完末块才能写出，存在不可重叠的头尾）。取保守的**无重叠叠加**作为上界：

```
t_C = t_A + t_B
```

实际单次发射耗时落在 `[max(t_A,t_B), t_A+t_B]` 区间内（完美重叠 ~ 零重叠）。

---

## 4. 代入数值（单 die：per_core=15.155 TFLOP/s，BW=1.6 TB/s，M=16，S=4096）

### 4.1 详细带宽搬运（口径 B，逐笔字节）

| kernel | blocks | A_in= blocks·M·K·2 | B_w= K·N·2 | C_out= M·N·4 | 合计 | t_B(µs) | 权重占比 |
|---|---:|---:|---:|---:|---:|---:|---:|
| q_proj | 20 | 3.28 MB | 52.43 MB | 0.33 MB | 56.03 MB | 35.0 | 93.6% |
| k_proj | 8 | 1.31 MB | 10.49 MB | 0.07 MB | 11.86 MB | 7.4 | 88.4% |
| v_proj | 8 | 1.31 MB | 10.49 MB | 0.07 MB | 11.86 MB | 7.4 | 88.4% |
| o_proj | 40 | 6.55 MB | 52.43 MB | 0.33 MB | 59.31 MB | 37.1 | 88.4% |
| gate_proj | 34 | 5.57 MB | 178.26 MB | 1.11 MB | 184.94 MB | 115.6 | 96.4% |
| up_proj | 34 | 5.57 MB | 178.26 MB | 1.11 MB | 184.94 MB | 115.6 | 96.4% |
| down_proj | 40 | 22.28 MB | 178.26 MB | 0.33 MB | 200.87 MB | 125.5 | 88.7% |
| qk_matmul | 20 | Q 0.08 | KV 16.78 | scores 10.49 | 27.34 MB | 17.1 | — |

举例 `gate_proj`：`A=34×16×5120×2=5.57MB`，`B=5120×17408×2=178.26MB`，`C=16×17408×4=1.11MB`，合计 184.94 MB，`t_B=184.94e6/1.6e12=115.6 µs`。

### 4.2 三口径汇总

| kernel | 口径A 算力(µs) | 口径B 带宽(µs) | 口径C 合并 A+B(µs) |
|---|---:|---:|---:|
| q_proj | 2.8 | 35.0 | **37.8** |
| k_proj | 1.4 | 7.4 | **8.8** |
| v_proj | 1.4 | 7.4 | **8.8** |
| o_proj | 2.3 | 37.1 | **39.4** |
| gate_proj | 7.8 | 115.6 | **123.4** |
| up_proj | 7.8 | 115.6 | **123.4** |
| down_proj | 7.8 | 125.5 | **133.4** |
| qk_matmul | 0.1 | 17.1 | **17.2** |
| **单层 7 proj 合计** | **31.4** | **443.6** | **475.0** |

---

## 5. 与实测对比

### 5.1 各 AIC kernel 实测用值（取自 `V200-benchmark/README.md` §1.2.1）

来源：`README.md` §1.2.1「a2a3 上各 kernel 执行时间」——`test_qwen3_decode.py -p a2a3 --enable-l2-swimlane` 一次实测，取 `merged_swimlane.json` 的 **AICore View**；simpler 版本 `61ba501`。「实例数」= AICore View 子任务事件数（SPMD 按子任务计）；单位 µs。下表只取 **AIC（cube）** 核函数：

| id | kernel | 实例数 | 总耗时 | **均值** | min | max | median | P90 | P99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | q_proj | 120 | 3127.8 | **26.06** | 22.00 | 46.80 | 22.63 | 41.54 | 46.74 |
| 2 | k_proj | 48 | 872.2 | **18.17** | 12.94 | 39.98 | 14.14 | 38.68 | 39.98 |
| 3 | v_proj | 48 | 858.9 | **17.89** | 13.12 | 38.78 | 14.05 | 37.14 | 38.78 |
| 6 | qk_matmul | 360 | 10567.0 | **29.35** | 2.58 | 65.96 | 28.04 | 50.40 | 63.62 |
| 8 | sv_matmul | 360 | 11395.2 | **31.65** | 2.78 | 61.82 | 30.59 | 55.34 | 59.74 |
| 13 | gate_proj | 204 | 19522.8 | **95.70** | 66.84 | 139.36 | 95.29 | 101.42 | 138.32 |
| 14 | up_proj | 204 | 19817.1 | **97.14** | 68.46 | 135.00 | 96.65 | 103.80 | 133.34 |
| 16 | down_proj | 240 | 17333.9 | **72.22** | 41.66 | 173.20 | 50.20 | 129.52 | 171.64 |
| 10/11 | out_proj_residual\* | 240 | 9779.0 | **40.75** | 12.90 | 141.66 | 26.14 | 116.78 | 133.22 |

> \* `out_proj_residual` 是 **mix**（aic+aiv 同 launch）任务，单实例时间 = `max(aic_dur, aiv_dur×2)`，故比纯 aic 的 o_proj 估值偏大。其余均为纯 aic（cube）。
> AIC 整体（README §1.2.2）：1584 个子任务、均值 **52.71 µs**、median 42.76、P99 134.62。

### 5.2 与本文 roofline 估算对比

实测来自 `swimlane_results/qwen3_dynamic_manual_scope_partial_spmd__a2a3__Case1_thread4_block24/merged_swimlane.json`（同口径另一次跑，AICore View，24 个 AIC 核）。每个 block 一条 `X` 事件，取**同名 kernel 所有 block 的平均时长**为「单块执行时间」；一次发射需 `waves = ⌈blocks/24⌉` 波，故「单次发射 wall」≈ `单块均时 × waves`。

| kernel | 口径C 估算(µs) | 实测单块均(µs) | waves | 实测发射 wall(µs) | 实测/口径C |
|---|---:|---:|---:|---:|---:|
| q_proj | 37.8 | 26.0 | 1 | 26.0 | 0.69 |
| k_proj | 8.8 | 18.2 | 1 | 18.2 | 2.07 |
| v_proj | 8.8 | 17.8 | 1 | 17.8 | 2.03 |
| o_proj | 39.4 | 29.7 | 2 | 59.5 | 1.51 |
| gate_proj | 123.4 | 68.0 | 2 | 136.0 | 1.10 |
| up_proj | 123.4 | 72.8 | 2 | 145.7 | 1.18 |
| down_proj | 133.4 | 80.7 | 2 | 161.3 | 1.21 |

> §5.1（README）的「均值」是**单子任务/单块**均时，与本表「实测单块均」同口径（数值略有差异因属不同次运行）；「单次发射 wall」需再乘 `waves`。

### 结论

1. **三口径关系**：A（算力，1–8 µs）≪ B（带宽，7–126 µs）< C=A+B。decode 恒由带宽决定，算力利用率仅 ~7%（脊点 237 FLOP/B ≫ 实际 17.6 FLOP/B）。
2. **带宽搬运分解**：权重读取 `K·N·2` 占 88–96%，是单次核函数耗时的绝对主因；激活重复读 `blocks·M·K·2` 次之（down_proj 因 K=17408 较大达 22 MB），输出写出最小。
3. **大 kernel 估得准**：gate/up/down 的实测发射 wall（136/146/161 µs）与口径 C（123/123/133 µs）误差仅 +10~21%，超出部分来自 kernel 启动、流水同步与 2 波调度间隙——验证了「带宽主导 + 无重叠叠加」模型的有效性。
4. **小 kernel 受固定开销主导**：k/v_proj 实测（~18 µs）反而 ≈ 口径 C 的 2 倍，因为搬运量太小（~12 MB / 7 µs），启动与同步等固定延迟（每核 ~10 µs 量级）占了主导，带宽模型在此处偏乐观。
5. 口径 A/B/C 均为理论值，未计 kernel 启动/同步/尾延迟；实测可作为带固定开销的真值参考。

### 附录：整 token 吞吐换算（带宽口径，仅供参考）

把口径 B 单层 ≈ 444 µs（含 qk/sv）乘 40 层：

| 部署 | HBM 字节/token | 带宽 | 延迟/token | 吞吐 |
|---|---:|---:|---:|---:|
| 单 die | ~27 GB | 1.6 TB/s | ≈ 17 ms | ~59 tok/s |
| 双 die TP | ~27 GB | 3.2 TB/s | ≈ 8.5 ms | ~118 tok/s |

---

## 6. 逐核函数详细计算过程

统一常量：`per_core = 16×16×16×2×1.85e9 = 15.155 TFLOP/s`，`BW = 1.6e12 B/s`，核数上限 24，`waves = ⌈blocks/24⌉`。
每个 kernel 给出：⓪ 参与张量（参数名/整体大小/单block实读/L1 tile）① FLOPs ② t_A（算力）③ 三笔搬运字节 ④ t_B（带宽）⑤ t_C=t_A+t_B ⑥ 实测对比。

> **「一次核函数要把整个权重 tensor 拿进来算吗？」** 是，但分两层、且流式：
> ① 跨 block 拆列——`blocks` 个核各算 `N_block=N/blocks` 列，各读权重 `K×N_block` 列，并集=整块权重、每列只读一次，故 `B_w=K·N·2` 即「一次发射读完整权重」，无重复；
> ② block 内沿 K 流式——每步 `TLOAD` 一个 `128×N_block` 的 L1 tile，算完即丢，L1 任一时刻只驻留一个小 tile（gate_proj 权重 178 MB ≫ 单核 L1 ~1 MB，必然流式）。
> 激活 A 被每个 block 各读一份（故 `A_in` 有 blocks 倍）；下表「整体大小」由源码 `GlobalTensor` 的 `Stride` 反推，「L1 tile」由 `Tile<>` 形状给出。M=16 为 `batch_padded`（有效 batch=`USER_BATCH_DYN`≤16，cube 仍按 16 行算）。
>
> **⓪「单block实读」与 ③ 的整 launch 字节是什么关系？** ⓪ 是**一个核**搬多少，③ 是**一次发射全芯片合计**搬多少。权重每列只被一个 block 读，故 `B_w(整launch) = 单block实读 × blocks`——如 q_proj：`2.621 MB × 20 block = 52.43 MB = K·N·2`，即 20 个 block 合起来把整块权重读一遍。`t_B` 用**整 launch 字节 ÷ 整 die 带宽**（1.6 TB/s 是 die 共享，20 个核同抢一条 HBM），不能拿单 block 字节配整 die 带宽——分子分母必须同口径。

---

### 6.1 q_proj　[16×5120]×[5120×5120]，blocks=20，cores=20，waves=1

```
⓪ 参与张量:
   A 激活  normed_tile (bf16)  整体 16×5120=0.164MB   单block实读 0.164MB   L1 tile 16×128
   B 权重  wq         (bf16)  整体 5120×5120=52.429MB 单block实读 5120×256=2.621MB L1 tile 128×256
   C 输出  q_proj_out    (fp32)  整体 16×5120=0.328MB   单block实写 16×256=16.4KB  L1 tile 16×256
① FLOPs = 2·M·K·N = 2×16×5120×5120 = 838,860,800 = 0.839 G
② t_A   = 0.839e9 / (20×15.155e12) = 2.77 µs
③ 搬运:
   A_in  = blocks·M·K·2 = 20×16×5120×2  = 3,276,800 B  = 3.277 MB   （激活被 20 个 block 各读一份）
   B_w   = K·N·2        = 5120×5120×2   = 52,428,800 B = 52.429 MB  （= 单block 2.621MB × 20 block；权重整体读一遍，主导项 93.6%）
   C_out = M·N·4        = 16×5120×4     = 327,680 B    = 0.328 MB   （FP32 输出）
   合计  = 56,033,280 B = 56.033 MB
④ t_B   = 56.033e6 / 1.6e12 = 35.02 µs
⑤ t_C   = 2.77 + 35.02 = 37.79 µs
⑥ 实测  : 单块 25.95 µs，wall(1波)=25.95 µs → 实测/C = 0.69
```

### 6.2 k_proj　[16×5120]×[5120×1024]，blocks=8，cores=8，waves=1

```
⓪ 参与张量:
   A 激活  normed_tile (bf16)  整体 16×5120=0.164MB    单block实读 0.164MB        L1 tile 16×128
   B 权重  wk         (bf16)  整体 5120×1024=10.486MB 单block实读 5120×128=1.311MB L1 tile 128×128
   C 输出  k_proj_out    (fp32)  整体 16×1024=65.5KB     单block实写 16×128=8.2KB     L1 tile 16×128
① FLOPs = 2×16×5120×1024 = 167,772,160 = 0.168 G
② t_A   = 0.168e9 / (8×15.155e12) = 1.38 µs
③ A_in  = 8×16×5120×2 = 1,310,720 B = 1.311 MB
   B_w   = 5120×1024×2 = 10,485,760 B = 10.486 MB（88.4%）
   C_out = 16×1024×4   = 65,536 B    = 0.066 MB
   合计  = 11,862,016 B = 11.862 MB
④ t_B   = 11.862e6 / 1.6e12 = 7.41 µs
⑤ t_C   = 1.38 + 7.41 = 8.80 µs
⑥ 实测  : 单块 18.20 µs，wall=18.20 µs → 实测/C = 2.07（搬运太小，固定开销主导）
```

### 6.3 v_proj　[16×5120]×[5120×1024]，blocks=8，cores=8，waves=1

```
⓪ 参与张量:
   A 激活  normed_tile (bf16)  整体 16×5120=0.164MB    单block实读 0.164MB        L1 tile 16×128
   B 权重  wv         (bf16)  整体 5120×1024=10.486MB 单block实读 5120×128=1.311MB L1 tile 128×128
   C 输出  v_proj_out    (fp32)  整体 16×1024=65.5KB     单block实写 16×128=8.2KB     L1 tile 16×128
① FLOPs = 2×16×5120×1024 = 0.168 G
② t_A   = 0.168e9 / (8×15.155e12) = 1.38 µs
③ A_in  = 8×16×5120×2 = 1.311 MB ; B_w = 5120×1024×2 = 10.486 MB ; C_out = 16×1024×4 = 0.066 MB
   合计  = 11.862 MB
④ t_B   = 7.41 µs   ⑤ t_C = 8.80 µs
⑥ 实测  : 单块 17.84 µs → 实测/C = 2.03
```

### 6.4 o_proj (out_proj_residual_aic)　[16×5120]×[5120×5120]，blocks=40，cores=24，waves=2

```
⓪ 参与张量:
   A 激活  attn_out       (bf16)  整体 16×5120=0.164MB    单block实读 0.164MB        L1 tile 16×128
   B 权重  wo            (bf16)  整体 5120×5120=52.429MB 单block实读 5120×128=1.311MB L1 tile 128×128
   C 输出  hidden_states (fp32)  整体 16×5120=0.328MB    单block实写 16×128=8.2KB     L1 tile 16×128
   （另有残差 resid1_tile 由 aiv 侧叠加，不计入本 aic 搬运）
① FLOPs = 2×16×5120×5120 = 0.839 G
② t_A   = 0.839e9 / (24×15.155e12) = 2.31 µs
③ A_in  = 40×16×5120×2 = 6,553,600 B = 6.554 MB  （40 block，激活重复读最多）
   B_w   = 5120×5120×2 = 52.429 MB（88.4%）
   C_out = 16×5120×4   = 0.328 MB
   合计  = 59.310 MB
④ t_B   = 59.310e6 / 1.6e12 = 37.07 µs
⑤ t_C   = 2.31 + 37.07 = 39.38 µs
⑥ 实测  : 单块 29.75 µs，wall(2波)=59.50 µs → 实测/C = 1.51
```

### 6.5 gate_proj　[16×5120]×[5120×17408]，blocks=34，cores=24，waves=2

```
⓪ 参与张量:
   A 激活  post_norm_tile (bf16)  整体 16×5120=0.164MB     单block实读 0.164MB         L1 tile 16×128
   B 权重  w_gate        (bf16)  整体 5120×17408=178.258MB 单block实读 5120×512=5.243MB L1 tile 128×512
   C 输出  gate_tile     (fp32)  整体 16×17408=1.114MB     单block实写 16×512=32.8KB    L1 tile 16×512
① FLOPs = 2×16×5120×17408 = 2,852,126,720 = 2.852 G
② t_A   = 2.852e9 / (24×15.155e12) = 7.84 µs
③ A_in  = 34×16×5120×2  = 5,570,560 B   = 5.571 MB
   B_w   = 5120×17408×2  = 178,257,920 B = 178.258 MB（96.4%，最大权重）
   C_out = 16×17408×4    = 1,114,112 B   = 1.114 MB
   合计  = 184,942,592 B = 184.943 MB
④ t_B   = 184.943e6 / 1.6e12 = 115.59 µs
⑤ t_C   = 7.84 + 115.59 = 123.43 µs
⑥ 实测  : 单块 68.01 µs，wall(2波)=136.01 µs → 实测/C = 1.10
```

### 6.6 up_proj　[16×5120]×[5120×17408]，blocks=34，cores=24，waves=2

```
⓪ 参与张量:
   A 激活  post_norm_tile (bf16)  整体 16×5120=0.164MB     单block实读 0.164MB         L1 tile 16×128
   B 权重  w_up          (bf16)  整体 5120×17408=178.258MB 单block实读 5120×512=5.243MB L1 tile 128×512
   C 输出  up_tile       (fp32)  整体 16×17408=1.114MB     单block实写 16×512=32.8KB    L1 tile 16×512
① FLOPs = 2×16×5120×17408 = 2.852 G
② t_A   = 2.852e9 / (24×15.155e12) = 7.84 µs
③ A_in = 5.571 MB ; B_w = 178.258 MB（96.4%）; C_out = 1.114 MB ; 合计 184.943 MB
④ t_B   = 115.59 µs   ⑤ t_C = 123.43 µs
⑥ 实测  : 单块 72.83 µs，wall(2波)=145.65 µs → 实测/C = 1.18
```

### 6.7 down_proj　[16×17408]×[17408×5120]，blocks=40，cores=24，waves=2

```
⓪ 参与张量:
   A 激活  mlp_tile (bf16)  整体 16×17408=0.557MB    单block实读 0.557MB         L1 tile 16×128
   B 权重  w_down   (bf16)  整体 17408×5120=178.258MB 单block实读 17408×128=4.456MB L1 tile 128×128
   C 输出  down_tile(fp32)  整体 16×5120=0.328MB     单block实写 16×128=8.2KB      L1 tile 16×128
① FLOPs = 2×16×17408×5120 = 2.852 G
② t_A   = 2.852e9 / (24×15.155e12) = 7.84 µs
③ A_in  = 40×16×17408×2 = 22,282,240 B = 22.282 MB  （K=17408 大，激活读最重）
   B_w   = 17408×5120×2 = 178.258 MB（88.7%）
   C_out = 16×5120×4    = 0.328 MB
   合计  = 200.868 MB
④ t_B   = 200.868e6 / 1.6e12 = 125.54 µs
⑤ t_C   = 7.84 + 125.54 = 133.38 µs
⑥ 实测  : 单块 80.65 µs，wall(2波)=161.31 µs → 实测/C = 1.21
```

### 6.8 qk_matmul　Q·Kᵀ，B=16，H_q=40，head_dim=128，S=4096，blocks=20，waves=1

```
⓪ 参与张量:
   Q 查询  all_q_padded    (bf16)  整体 [B,H_q,128]   单block实读 16×128=0.041MB  L1 tile 16×128
   K 缓存  k_cache         (bf16)  整体 [KV_ROWS,128]  按 block_table 取 128×128 块  整 S 读 8.389MB
           （索引表 block_table, int32）
   分数    all_raw_scores (fp32)  整体 [B,H_q,S]      写 10.486MB                 L1 tile 16×128
① FLOPs = 2·B·H_q·S·d = 2×16×40×4096×128 = 0.671 G
② t_A   = 0.671e9 / (20×15.155e12) = 2.21 µs
③ Q_in  = blocks·M·d·2     = 20×16×128×2      = 81,920 B     = 0.082 MB
   KV    = 2·S·H_kv·d·2     = 2×4096×8×128×2   = 16,777,216 B = 16.777 MB（读 K、V，主导）
   scores= B·H_q·S·4        = 16×40×4096×4     = 10,485,760 B = 10.486 MB（写 FP32 分数）
   合计  = 27.345 MB
④ t_B   = 27.345e6 / 1.6e12 = 17.09 µs
⑤ t_C   = 2.21 + 17.09 = 19.30 µs
⑥ 实测  : 单块 26.35 µs → 实测/C = 1.37（随 S 线性增长）
```

### 6.9 sv_matmul　Score·V，B=16，H_q=40，head_dim=128，S=4096，blocks=20，waves=1

```
⓪ 参与张量:
   概率    all_exp_padded (bf16)  整体 [B,H_q,S]      读 5.243MB                  L1 tile 16×128
   V 缓存  v_cache        (bf16)  整体 [KV_ROWS,128]  按 block_table 取 128×128 块  整 S 读 8.389MB
           （索引表 block_table, int32）
   输出    all_oi_tmp    (fp32)  整体 [B,H_q,128]    写 0.328MB                  L1 tile 16×128
① FLOPs = 2·B·H_q·S·d = 2×16×40×4096×128 = 0.671 G
② t_A   = 0.671e9 / (20×15.155e12) = 2.21 µs
③ scores_in = B·H_q·S·2 = 16×40×4096×2 = 5.243 MB（读概率，BF16）
   V_in      = S·H_kv·d·2 = 4096×8×128×2 = 8.389 MB（读 V）
   out       = B·H_q·d·4  = 16×40×128×4  = 0.328 MB（写注意力输出）
   合计 ≈ 13.96 MB（KV 与 qk 共享则计 16.78MB）
④ t_B   ≈ 8.7 ~ 17.1 µs（取决于 V 是否复用 qk 已读）
⑤ t_C   ≈ 11 ~ 19 µs
⑥ 实测  : 单块 28.62 µs → 与 qk 同量级
```

---

## 7. 什么情况下是计算瓶颈（而非带宽瓶颈）

### 7.1 判据：算术强度 vs 脊点

是计算瓶颈还是带宽瓶颈，由**算术强度** AI = FLOPs/Bytes 与硬件**脊点** ridge = P/BW 比较决定：

```
ridge = P / BW = 378.9 TFLOP/s / 1.6 TB/s = 237 FLOP/B   （单 die；整卡双 die 同为 237）
AI > ridge → 计算瓶颈（t_A 主导）
AI < ridge → 带宽瓶颈（t_B 主导）
```

### 7.2 关键结论：权重主导的 GEMM，AI ≈ M（处理的行数）

对一次发射、权重字节主导（`K·N·2` ≫ 激活、输出）的投影 GEMM：

```
AI = 2·M·K·N / (K·N·2) = M
```

也就是说**算术强度≈ 一次处理的行数 M**（= batch × 每步 token 数）。物理含义：权重从 HBM 读一次，要被多少次乘加复用 ≈ M 次；M 越大，权重越「摊薄」。于是**判据简化为**：

```
M < 237 → 带宽瓶颈      M > 237 → 计算瓶颈
```

本文 decode 的 M=16（batch_padded）≪ 237，故全程带宽瓶颈。要跨到计算瓶颈，需要 M ≳ 237。

### 7.3 具体会变成计算瓶颈的样例

| 场景 | M = batch×token | AI≈M | 瓶颈 |
|---|---:|---:|---|
| 本文 decode（batch≤16，每步 1 token） | 16 | 15 | 带宽 |
| 大 batch decode（batch≥256） | ≥256 | ≥256 | **计算** |
| **Prefill / 长 prompt**（batch1 × 2048 token 一次算） | 2048 | 931 | **计算** |
| 训练 / 大 batch 前向 | 数千 | 数千 | **计算** |

**算例：q_proj 在 prefill，M=2048（一条 2048-token prompt）**

```
FLOPs = 2×2048×5120×5120 = 107.4 G
Bytes = 权重 52.43MB + 激活 2048×5120×2=20.97MB + 输出 2048×5120×4=41.94MB = 115.34 MB
AI    = 107.4e9 / 115.34e6 = 931 FLOP/B  >  237  → 计算瓶颈
t_A   = 107.4e9 / 378.9e12 = 283.4 µs
t_B   = 115.34e6 / 1.6e12  =  72.1 µs
→ t_A 主导，t = max ≈ 283 µs（算力受限，比带宽侧大 3.9×）
```

对比同一个 q_proj 在 decode（M=16）：AI≈15、t_B=35µs 主导。**同一个 kernel，M 从 16 升到 2048，瓶颈就从带宽翻转为计算**——区别只在「一次喂多少行」。

### 7.4 注意力的情况

注意力 qk/sv 的 AI 不是 ≈M，而是随 KV 长度 S 增长（`AI ≈ S/(常数)`）：decode 每步只 1 个 query、S 行 KV，AI 低 → 带宽（KV 读取）主导；但 **prefill 阶段 S 很大、且 query 也有 S 行**，qk=`[S×d]×[d×S]`、sv=`[S×S]×[S×d]`，AI 随 S 线性上升，长序列 prefill 下注意力也会转入计算瓶颈。

### 7.5 其它影响因素

- **权重量化**（W4/W8）：权重字节减半/四分之一 → `t_B` 下降、脊点右移，decode 带宽压力缓解，但 M 很小时通常仍是带宽瓶颈（只是更轻）。
- **cube 利用率**：M=16 时只填了 cube 16×16 阵列的 16 行，本身就未跑满；M≥256 才能让 cube 行向量满载，这也是大 M 才进入计算瓶颈的另一面。

---

## 8. paged_attention_unroll 估算与实测

> 路径：`V200-benchmark/paged_attention/paged_attention_unroll/`　硬件同 A3 单 die。
> 与上文 qwen3 投影不同，这是**分页注意力**（paged attention），KV 按 block_table 离散索引取入；下面单独估算并对实测。

### 8.1 参数（Case1, a2a3，源自 `test_paged_attention_unroll.py` + orchestration）

```
batch=480  num_heads=16  kv_head_num=1（MQA，16 个 q 头共享 1 份 KV）
head_dim=128  block_size=128  context_len≈8192  dtype=bf16
block_dim=24（24 个 AIC 核）  aicpu_thread_num=4  N_UNROLL=64
```

两个 aic 核函数（Case1 模板 `<M,K,N>=<16,128,128>`，每次发射处理 `n_blocks` 个 KV 块）：

| kernel | 参数名 | 计算 | M | K | N | n_blocks |
|---|---|---|---:|---:|---:|---:|
| `aic_qk_matmul` (QK) | `qi`,`key_cache`,`sij` | qi·kjᵀ 逐块 | 16(q_tile=16头) | 128(head_dim) | 128(block_size) | 64 |
| `aic_pv_matmul` (PV) | `pij`,`value_cache`,`oi` | SplitK Σ pij·vj | 16 | 128 | 128 | 64 |

> 一次发射 = 一个 batch 的注意力（16 个 q 头 × 64 个 KV 块），跑在**单个核**上；480 个 batch → 这些发射分摊到 24 核。`n_blocks=64` 对应 context=8192（满 N_UNROLL）。

### 8.2 单次发射估算（n_blocks=64，单核 per_core=15.155 TFLOP/s，BW=1.6 TB/s）

**QK：**
```
① FLOPs = n_blocks·2·M·K·N = 64×2×16×128×128 = 33.55 MFLOP
② t_A   = 33.55e6 / 15.155e12 = 2.21 µs   （单核算力）
③ 搬运:
   qi  = M·K·2            = 16×128×2          = 4 KB     （循环外只读一次）
   key = n_blocks·K·N·2   = 64×128×128×2      = 2.097 MB （主导，按 block_table 离散取）
   sij = n_blocks·M·N·4   = 64×16×128×4       = 0.524 MB （写 FP32 分数）
   合计 ≈ 2.625 MB
④ t_B   = 2.625e6 / 1.6e12 = 1.64 µs   （理想满带宽下界）
   AI   = 33.55e6 / 2.625e6 = 12.8 FLOP/B  ≪ 脊点 237 → 带宽侧
```

**PV：**
```
① FLOPs = 64×2×16×128×128 = 33.55 MFLOP
② t_A   = 2.21 µs
③ pij = 64×16×128×2 = 0.262 MB ; value = 64×128×128×2 = 2.097 MB ; oi = 16×128×4 = 8 KB ; 合计 ≈ 2.367 MB
④ t_B   = 2.367e6 / 1.6e12 = 1.48 µs ;  AI = 14.2 FLOP/B
```

按 roofline，单次发射 ≈ 2 µs（算力/带宽都只要 1~2 µs）。

### 8.3 实测（`simpler/outputs/TestPagedAttentionUnroll_a2a3/merged_swimlane.json`）

AICore View：24 个 AIC 核，各 ~95% 忙；整个 paged attention 解码 **wall ≈ 2.25 ms**。逐 kernel：

| kernel | 发射次数 | 实测中位(µs) | 实测均值(µs) | 每块≈(µs) | roofline估(µs) | 实测/估 |
|---|---:|---:|---:|---:|---:|---:|
| QK | 960 | 55.3 | 73.5 | 0.86 | ~2 | **~28×** |
| PV | 960 | 57.7 | 76.3 | 0.90 | ~2 | **~29×** |
| SF (aiv softmax) | 960 | 61.1 | 61.3 | — | — | — |
| UP (aiv online-update) | 960 | 3.7 | 4.6 | — | — | — |

> 960 ≈ 480 batch × 2 unroll 组（部分序列 > 8192 token，按 N_UNROLL=64 切成 2 组）。

### 8.4 为什么实测比 roofline 大 ~28×：单任务是「延迟瓶颈」

单次发射的 roofline（2 µs）严重低估，因为它假设搬运与计算完全流水重叠、且按峰值带宽。实际**每个 KV 块约 0.86 µs**（55µs/64 块），瓶颈是**延迟**不是吞吐：

1. **逐块同步**：`aic_qk_matmul.cpp` 每个块迭代末尾 `pipe_barrier(PIPE_ALL)`，把 `TLOAD(kj)→TMATMUL→TSTORE` 三段串起来、不跨块重叠 → 每块都要付一次完整的「读 32KB + 算 + 写」往返延迟。0.86 µs/块 正是一次非重叠 HBM 访问延迟的量级（≈ 25× 单块算力 34 ns、≈ 42× 单块理想带宽 20 ns）。
2. **离散分页 KV**：每个 `kj`/`vj`（32 KB）由 `block_table` 索引在 HBM 里**随机分散**取，远达不到 1.6 TB/s 顺序峰值带宽。
3. **单核低占用**：M=16 只填了 cube 16×16 阵列的 16 行，且一次发射只用一个核。

### 8.5 但 24 核「聚合」又回到带宽瓶颈

把所有发射的 KV 流量加总：QK 读 key ≈ 960×2.097MB ≈ 2.0 GB，PV 读 value ≈ 2.0 GB，合计 **~4 GB KV**。除以单 die 带宽：

```
t_聚合 ≈ 4 GB / 1.6 TB/s ≈ 2.5 ms   （vs 实测 wall 2.25 ms）
```

两者吻合：**单个任务看是延迟瓶颈（55µs，~28× roofline），但 24 个核并发把 HBM 带宽吃满后，整体 wall ≈ KV 总量 / 带宽 ≈ 2.25 ms**，即聚合层面回到带宽瓶颈（实测略低 → 有效带宽 ~1.8 TB/s，或部分序列 < 64 块）。

> 启示：paged attention 的 roofline 要在**聚合层面**（总 KV / 带宽）看才准；单任务的 t_A/t_B 是松下界，真实单任务由逐块同步 + 离散访存的**延迟**决定。去掉逐块 `PIPE_ALL`、改双缓冲跨块重叠（PV 已部分这么做），单任务有望显著下降。
> `TestPagedAttentionUnrollManualScope_a2a3` 变体数值相近（QK 中位 55.1µs、PV 58.2µs、wall ≈ 2.24 ms）。

---

## 9. 单层 HBM / L2 数据流量与时间分解（全 AIC + AIV 核函数）

前面 `t_B` 只数「下到 HBM」的字节。这一节按**一个解码层**（18 个核函数）把每一笔搬运按存储层级拆开，分别算 **HBM 侧（memory-bound）时间** 与 **L2 侧读写时间**。

### 9.1 一层的核函数流水（生产者 → 消费者）

```
rmsnorm(aiv) → q/k/v_proj(aic) → qk_norm(aiv) → rope_kv_cache(aiv)
 → qk_matmul(aic) → softmax(aiv) → sv_matmul(aic) → online_softmax(aiv)
 → out_proj_residual(aic+aiv) → post_rmsnorm(aiv)
 → gate_proj(aic) → up_proj(aic) → silu(aiv) → down_proj(aic) → down_proj_residual(aiv)
```

张量分两类：**① 权重 / KV cache**——大、读一次、无复用，**必须走 HBM**；**② 中间激活**（normed_tile、q/k/v_proj、scores、gate/up、mlp、resid1…）——小，上一个核产出、下一个核消费，**理应留在片上 L2**，产出=`L1/UB→L2` 写、消费=`L2→L1/UB` 读，只有被逐出才落 HBM。

### 9.2 单层四类搬运（decode：M=16，hidden=5120，inter=17408，S=4096）

| 类别 | 路径 | 内容（每层） | 字节/层 |
|---|---|---|---:|
| **A 必读** | HBM → L1/UB | 7 个大权重 + 若干小权重 + KV(K,V) | **677.4 MB** |
| **B 必写** | L1/UB → HBM | KV cache 写(新 token K,V) + 层输出 hidden | **0.23 MB** |
| **C 中间写** | L1/UB → L2 | ~20 个中间激活产出 | **20.8 MB** |
| **D 中间读** | L2 → L1/UB | ~24 次中间激活消费（含复用） | **21.5 MB** |

- **A 绝对主导**：权重 660.6 MB/层（gate/up/down 各 178.3 MB）+ KV 16.8 MB/层。
- **C/D 是片上往返**（最大的中间量：`all_raw_scores` 10.5 MB、`all_exp_padded` 5.24 MB、`gate_tile`/`up_tile` 各 1.11 MB），**若留 L2 则不占 HBM**。

### 9.3 各级读写速度（本机 `Ascend910B1.ini` `[AICoreMemoryRates]`，单位 B/cycle/核 @1.85 GHz）

| 路径 | B/cyc/核 | 单核 | 24 核聚合 | 对 HBM 倍数 |
|---|---:|---:|---:|---:|
| HBM(ddr) 读/写 | 32 | 59.2 GB/s | **1.42 TB/s** | 1× |
| **L2 读** (`l2_read_rate`) | 110 | 203.5 GB/s | **4.88 TB/s** | **3.4×** |
| **L2 写** (`l2_write_rate`) | 86 | 159.1 GB/s | **3.82 TB/s** | **2.7×** |
| UB→L2 写 (向量) | 64 | 118.4 GB/s | 2.84 TB/s | 2.0× |
| L1→L0A | 512 | 947 GB/s | — | 16× |

> **只有「B/cyc/核」列是 ini 原始值**（`ddr_rate=32`、`l2_read_rate=110`、`l2_write_rate=86`、`ub_to_l2_rate=64`、`l1_to_l0_a_rate=512`）；其余列均为换算：单核 GB/s = `B/cyc × 1.85e9`（如 32×1.85e9=59.2 GB/s），24 核聚合 = `单核 × 24`（如 59.2×24=1.42 TB/s）。

> ini 的 ddr 聚合 1.42 TB/s 与公开 ~1.6 TB/s 同量级（本文 HBM 仍取 1.6）。**L2 比 HBM 快 ~3×（读 3.4×、写 2.7×）**，但容量只有 192 MB。

### 9.4 单层时间：memory-bound（HBM）vs L2 读写

```
HBM 侧（必读 A 主导，t_B）:
   t_HBM = 677.4 MB / 1.6 TB/s = 423 µs            （按 ini 1.42 TB/s = 477 µs）

L2 侧（中间激活留片上，C/D）:
   t_L2写 = 20.8 MB / 3.82 TB/s = 5.4 µs
   t_L2读 = 21.5 MB / 4.88 TB/s = 4.4 µs
   t_L2  ≈ 9.8 µs
```

**单层 ≈ 423 µs，由 HBM 读权重决定**。L2 中间量往返即便不重叠也只 ~10 µs（占 ~2.3%），且与 HBM 走不同通路、可被 423 µs 完全掩盖。算力侧 t_A≈31 µs（§4.2）同样被掩盖。

> 结论：单层是**彻底的 HBM-memory-bound（423 µs）**——L2 快 3×、但要搬的中间量太少（21 MB vs 权重 660 MB），省不出时间；t_A、t_L2 都藏在 t_HBM 之下。

### 9.5 L2 频繁失效的风险点（单层内）

L2 只有 **192 MB**，关键看「中间量在产出→消费之间，会不会被中途的大权重流冲掉」。一旦被逐出，该中间量就从「~4–5 TB/s 的 L2 往返」掉到「1.6 TB/s 的 HBM 往返」（慢 ~3×）：

| 风险点 | 机理 | 后果 |
|---|---|---|
| **① 权重流冲刷 L2** | 每个 gate/up/down 权重 **178 MB**，单次就接近/吃满 192 MB L2，一遍流过把 L2 里所有中间量挤掉；权重读一次无复用，对 L2 零收益、反成「污染源」 | L2 对权重等于旁路；并殃及共存的中间量 |
| **② resid1_tile 跨整个 FFN** | `out_proj`(步10)产出残差 → `down_proj_residual`(步17)才消费，**中间 gate+up+down = 534 MB 权重流过** → resid1（0.328 MB）必被逐出 | 由 L2 往返掉到 HBM 往返（+0.66 MB/层、慢 3×），确定性 miss |
| **③ normed_tile 跨 q/k/v_proj** | 产出后 wq/wk/wv（73 MB）流过才被消费 | 0.164 MB 可能被逐出（消费靠前，风险中等） |
| **④ 注意力大中间量** | `all_raw_scores` 10.5 MB / `all_exp_padded` 5.24 MB，若与其它流式交错则有逐出风险；正常紧邻 softmax/sv 则留 L2 | 取决于调度紧凑度 |
| **⑤ KV cache（长上下文）** | 每层 KV 16.8 MB@S=4096 且随 S 线性增长，无跨层复用，超 L2 复用窗口 | 本就走 HBM，L2 救不了（paged 还更糟，见 §8.4） |

### 9.6 各 AIC kernel 数据拷贝的分级耗时（**单块 = 单核**，用 §9.3 速率）

**口径**：一个 block 跑在一个核上，**只搬自己那一块**——只读输出的 `N_block = N/blocks` 列权重，不是整块权重。每块字节：

```
N_block   = N / blocks                              （这个 block 负责的输出列数）
右(权重)   = K × N_block × 2   字节                  （沿全 K、只取 N_block 列；GM→L1 / L1→L0B）
左(激活)   = M × K × 2         字节                  （整列激活，被本块用；L2→L1 / L1→L0A）
输出       = M × N_block × 4   字节                  （FP32；L0C→UB / UB→L2）
每级耗时    = 该级字节 / 该级单核速率(GB/s)            （速率见 §9.3 单核列）
```

**(a) A×B=C 各是什么、谁能共享、各搬多久**

每个算子 `C = A × B`，但 **A、B 各是什么、谁能 L2 共享因算子而异**（共享 = 多个核读**同一份**数据，先入 L2 后各核复用，省 HBM）。表内每个 A/B/C 都标 **✅可共享 / ❌每核独有**：

- **投影**（q/k/v/o/gate/up/down）：A=**激活**（所有核读同一份 `[M,K]`）✅；B=**权重**（各核取不同 `N/blocks` 列）❌→瓶颈；C=**输出**（各核写自己列）❌。
- **注意力**（qk/sv）：**共享的反过来是 B**——A=Q/P（各核不同 q 头）❌；B=K/V（GQA 下 40 q 头 / 8 kv 头，**同 kv-head 的多个核读同一份 K/V**）✅组内共享；C=分数/输出 ❌。

| kernel | A 左 | B 右 | C 输出 | A:L2→L1 | **B:GM→L1** | C:写出 | 拷贝总耗时(A+B+C) | kernel 实测均值\*\*\* |
|---|---|---|---|---:|---:|---:|---:|---:|
| q_proj | ✅`normed_tile`(激活)[16×5120] | ❌`wq`(权重)[5120×**256**]=2.62 MB | ❌`q_proj_out`[16×256] | 0.8 | **44.3** | 0.1 | **45.2** | 26.1 |
| k_proj | ✅`normed_tile`[16×5120] | ❌`wk`[5120×**128**]=1.31 MB | ❌`k_proj_out`[16×128] | 0.8 | **22.1** | 0.1 | **23.0** | 18.2 |
| v_proj | ✅`normed_tile`[16×5120] | ❌`wv`[5120×**128**]=1.31 MB | ❌`v_proj_out`[16×128] | 0.8 | **22.1** | 0.1 | **23.0** | 17.9 |
| o_proj | ✅`attn_out`[16×5120] | ❌`wo`[5120×**128**]=1.31 MB | ❌`hidden_states`[16×128] | 0.8 | **22.1** | 0.1 | **23.0** | 40.8\* |
| gate_proj | ✅`post_norm_tile`[16×5120] | ❌`w_gate`[5120×**512**]=5.24 MB | ❌`gate_tile`[16×512] | 0.8 | **88.6** | 0.3 | **89.7** | 95.7 |
| up_proj | ✅`post_norm_tile`[16×5120] | ❌`w_up`[5120×**512**]=5.24 MB | ❌`up_tile`[16×512] | 0.8 | **88.6** | 0.3 | **89.7** | 97.1 |
| down_proj | ✅`mlp_tile`[16×17408] | ❌`w_down`[17408×**128**]=4.46 MB | ❌`down_tile`[16×128] | 2.7 | **75.3** | 0.1 | **78.1** | 72.2 |
| qk_matmul | ❌`all_q_padded`(Q) 8 KB | ✅`k_cache`(K) 每核 **2.10 MB**(∝seq, GQA组内共享) | ❌`all_raw_scores`≈0.52 MB | ~0 | **35.4** | 4.4 | **39.8** | 29.4\*\* |
| sv_matmul | ❌`all_exp_padded`(P)≈0.26 MB | ✅`v_cache`(V) 每核 **2.10 MB**(∝seq, GQA组内共享) | ❌`all_oi_tmp` 16 KB 累加 | 1.3 | **35.4** | 0.1 | **36.8** | 31.6\*\* |

（µs，单块=单核；投影 `N_block = N/blocks` 即 B 的列数加粗。「拷贝总耗时」= A+B+C 三笔之和，**无重叠上界**；流水重叠后实际 ≈ 瓶颈 `B:GM→L1`，故权重读几乎决定一切。\*\*\* 「kernel 实测均值」取自 §5.1 / `README.md` §1.2.1 的子任务均值，**含计算+同步**，与「拷贝总耗时」同口径对照：FFN 两者吻合（拷贝主导），\* o_proj 为 mix(含 aiv)；\*\* 注意力 GM→L1 估值 35.4 µs 已与实测 29~32 µs 同量级——若一个 SPMD 块的 2 个 q 头落在同一 kv-head（GQA，40q/8kv），K/V 仅 1 份下 HBM(≈1.05 MB→17.7 µs)、另一份命中 L2，实际落在 ~22~40 µs 区间，正好夹住实测。）

> **「可 L2 共享」的具体所指**（= 多核读同一份、先入 L2 后各核复用，省 HBM）：
> - **投影**：共享的是 **A=激活**（normed_tile / post_norm_tile / mlp_tile / attn_out）——所有核读同一份 `[M,K]`，HBM 只读 1 份、各核 L2→L1（0.8~2.7 µs）；权重 B、输出 C 每核独有。
> - **注意力**：共享的是 **B=K/V**（GQA：40 q 头 / 8 kv 头 → 处理同一 kv-head 的多个核读同一份 K/V，L2 命中）；A=Q/P 各核按头不同、C 各核独有。
> **注意力的 SplitK 形态**：每核 = 2 个 q 头，沿 `ctx_blocks = seq_len/128` 个 KV 块**循环累加**（O += Pᵢ·Vᵢ，结果留在 L0C，不回读 HBM）；故每核要把自己这 2 头的 K/V 沿**整条 seq** 读进来 = `2×seq_len×head_dim×2` = 2.10 MB（∝seq_len）——这正是把它**按行（seq）切**、而非按列均分的原因。

**(b) 每级耗时 = 字节 ÷ 单核速率**（GB/s/核：GM→L1 **59.2**、L2→L1 **203.5**、L1→L0A **947**、L1→L0B **473.6**、L0C→UB **473.6**、UB→L2 **118.4**）：

| kernel | **GM→L1 权重**= 右÷59.2 | L2→L1 激活= 左÷203.5 | L1→L0A= 左÷947 | L1→L0B= 右÷473.6 | L0C→UB= 出÷473.6 | UB→L2= 出÷118.4 | 瓶颈 | README |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| q_proj | **44.3** | 0.8 | 0.2 | 5.5 | 0.0 | 0.1 | GM→L1 44.3 | 26.1 |
| k_proj | **22.1** | 0.8 | 0.2 | 2.8 | 0.0 | 0.1 | GM→L1 22.1 | 18.2 |
| v_proj | **22.1** | 0.8 | 0.2 | 2.8 | 0.0 | 0.1 | GM→L1 22.1 | 17.9 |
| o_proj | **22.1** | 0.8 | 0.2 | 2.8 | 0.0 | 0.1 | GM→L1 22.1 | 40.8\* |
| gate_proj | **88.6** | 0.8 | 0.2 | 11.1 | 0.1 | 0.3 | GM→L1 88.6 | 95.7 |
| up_proj | **88.6** | 0.8 | 0.2 | 11.1 | 0.1 | 0.3 | GM→L1 88.6 | 97.1 |
| down_proj | **75.3** | 2.7 | 0.6 | 9.4 | 0.0 | 0.1 | GM→L1 75.3 | 72.2 |
| qk_matmul\*\* | **35.4** | 0.0 | 0.0 | 4.4 | 1.1 | 4.4 | GM→L1 35.4 | 29.4 |
| sv_matmul\*\* | **35.4** | 1.3 | 0.3 | 4.4 | 0.0 | 0.1 | GM→L1 35.4 | 31.6 |

（µs。\* o_proj README 为 mix(aic+aiv)，含向量部分；\*\* 注意力的 B 不是 `N/blocks` 列权重，而是 **`k_cache`/`v_cache` 沿 seq_len 切**：每核 1 个 SPMD 块 = 2 个 q 头，循环 `ctx_blocks = seq_len/128` 个 KV 块、每块 `[128×128]·2`=32 KB → 每核 K/V = `2头 × seq_len × head_dim × 2` = 2×4096×128×2 = **2.10 MB**，**随 seq_len 线性增长**。GM→L1 = 2.10 MB ÷ 59.2 = 35.4 µs，已与实测 29~32 µs 同量级；GQA 下 2 头若同 kv-head 则 K/V 仅 ≈1.05 MB 下 HBM（→17.7 µs，另一份命中 L2），实际 ~22~35 µs。）

**逐数怎么算的（以 gate_proj 一个 block 为例）**

```
N_block = 17408 / 34 = 512
右权重/块 = K × N_block × 2 = 5120 × 512 × 2 = 5,242,880 B = 5.24 MB
左激活/块 = M × K × 2       = 16 × 5120 × 2   = 163,840 B  = 0.164 MB
输出/块   = M × N_block × 4 = 16 × 512 × 4    = 32,768 B   = 0.0328 MB

GM→L1 权重 = 5.24 MB / 59.2 GB/s   = 88.6 µs   ← 瓶颈（单核 HBM 只有 59.2 GB/s）
L1→L0B 权重 = 5.24 MB / 473.6 GB/s = 11.1 µs   （同样的权重，进 L0B 快 8×）
L2→L1 激活 = 0.164 MB / 203.5 GB/s = 0.8 µs
L1→L0A 激活 = 0.164 MB / 947 GB/s  = 0.2 µs
L0C→UB 输出 = 0.0328 MB / 473.6 GB/s = 0.07 µs
UB→L2 输出 = 0.0328 MB / 118.4 GB/s = 0.28 µs
→ 一块拷贝时间 ≈ max(各级) = 88.6 µs（流水重叠），与 README 95.7 µs 吻合
```

**(c) 每个 kernel 的拷贝总用时**

一次发射有 `blocks` 个块、`24` 个核并行，需 `waves = ⌈blocks/24⌉` 波，故**一次发射的拷贝 wall ≈ 单块 GM→L1 × waves**；整段 decode 该 kernel 共发射 `launches` 次（README 实例数 ÷ blocks），**全程总时 = README 均值 × 实例数**。

| kernel | 整块权重 K·N·2 | cores | waves | **单次发射拷贝**= 单块×waves | README 发射 wall(=均值×waves) | launches | README 全程总 |
|---|---:|---:|---:|---:|---:|---:|---:|
| q_proj | 52.4 MB | 20 | 1 | **44.3 µs** | 26.1 | 6 | 3.1 ms |
| k_proj | 10.5 MB | 8 | 1 | **22.1 µs** | 18.2 | 6 | 0.9 ms |
| v_proj | 10.5 MB | 8 | 1 | **22.1 µs** | 17.9 | 6 | 0.9 ms |
| o_proj | 52.4 MB | 24 | 2 | **44.3 µs** | 81.5\* | 6 | 9.8 ms\* |
| gate_proj | 178.3 MB | 24 | 2 | **177.1 µs** | 191.4 | 6 | 19.5 ms |
| up_proj | 178.3 MB | 24 | 2 | **177.1 µs** | 194.3 | 6 | 19.8 ms |
| down_proj | 178.3 MB | 24 | 2 | **150.6 µs** | 144.4 | 6 | 17.3 ms |
| qk_matmul | 每核 K 2.10 MB(∝seq) | 20 | 1 | **35.4 µs** | 29.4\*\* | 18 | 10.6 ms |
| sv_matmul | 每核 V 2.10 MB(∝seq) | 20 | 1 | **35.4 µs** | 31.6\*\* | 18 | 11.4 ms |

（\* o_proj 为 mix(aic+aiv)；\*\* 注意力每核 K/V = `2头×seq_len×head_dim×2`=2.10 MB（∝seq_len），GM→L1=2.10÷59.2=35.4 µs，GQA 组内共享后实际 ~22~35 µs，与实测 29/32 µs 吻合。「全程总」= README §1.2.1 该行总耗时。）

- **FFN 单次发射**估 177/177/151 µs ≈ README 实测 191/194/144 µs，**吻合**——一次 gate 发射就要 ~180 µs 搬 178 MB 权重（24 核并行、2 波）。
- **注意力单次发射**估 35.4 µs ≈ 实测 29/32 µs——每核搬 2.10 MB K/V（∝seq_len），seq 越长越久。
- **全程总时**（README 整段 decode 求和）：gate/up/down 各 ~19/20/17 ms 最重，三者合计 ~57 ms 占 AIC 拷贝大头；q/k/v 各 ~1–3 ms，注意力 qk/sv 各 ~11 ms（实例多）。

**结论**
- GM→L1 **确实只算了当前核那一块**（gate 只搬 5.24 MB，不是整块 178 MB）；它仍是几十µs，是因为**单核 HBM 带宽只有 59.2 GB/s**（= ini `ddr_rate=32` B/cyc × 1.85 GHz）。5.24 MB ÷ 59.2 GB/s = 88.6 µs，就这么来的。
- **同一份权重**进 L1 慢（GM→L1 59 GB/s），进 L0B 快（L1→L0B 474 GB/s，快 8×）——所以 `L1→L0B`、`L1→L0A`、`L0C→UB`、`UB→L2` 这些**片上级别都不是瓶颈**，被 GM→L1 完全掩盖。
- 三类都对得上：**FFN** 估 88.6/88.6/75.3 µs ≈ README 95.7/97.1/72.2；**注意力** 估 35.4 µs ≈ README 29.4/31.6（K/V 沿 seq 切、每核 2.10 MB ∝seq_len，GQA 组内共享后落 ~22~35 µs）；**小投影** 估 22~44 µs 略高于实测 18~26（激活/权重小，L2 部分命中）。整体证实「拷贝时间 ≈ 单核 GM→L1 读」。

> 字节上中间量很小（即便全 thrash 也只给 HBM +~5%），所以单层 423 µs 几乎不变；L2 命中与否主要影响那 ~10 µs 的中间量往返快慢，不改变「HBM-bound」的大局。真要保护的是 **resid1_tile**（跨整个 FFN 存活），靠编排把生产者/消费者贴近、别让大权重流插在中间。

---

### 参考来源
- 本机实测：`npu-smi info`（task-submit @ device 4，Ascend910 / A3）
- paged attention 实测 trace：`simpler/outputs/TestPagedAttentionUnroll_a2a3/merged_swimlane.json`（及 `…ManualScope_a2a3`）
- 实测 trace：`swimlane_results/qwen3_dynamic_manual_scope_partial_spmd__a2a3__Case1_thread4_block24/merged_swimlane.json`
- [Huawei Ascend 910C — Awesome Agents](https://awesomeagents.ai/hardware/huawei-ascend-910c/)
- [SemiAnalysis of Huawei CloudMatrix and the 910C — FiberMall](https://www.fibermall.com/blog/semianalysis-of-huawei-cloudmatrix-910c.htm)
