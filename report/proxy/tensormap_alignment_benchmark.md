# TensorMap Alignment Benchmark

Orchestration-only timing (`ORCHESTRATION_TIME=1`, scheduler not started).


## Baseline

Runs per case: 3

| Case | Variant | Runs | elapsed_ns (median) | min-max ns | task_tp MTasks/s | subtask_tp MTasks/s | pool_high_water |
|------|---------|------|---------------------|------------|------------------|---------------------|-----------------|
| qwen3_dynamic_tensormap.h | tier=0 | 3 | 4152020 | -4262061 | 0.745661 | 0.745661 | 4242 |
| qwen3_dynamic_tensormap.h | tier=2 | 3 | 492685 | -528325 | 1.753656 | 6.283934 | 1290 |
| qwen3_dynamic_tensormap.h | tier=4 | 3 | 199552 | -203462 | 2.615860 | 15.514753 | 894 |
| paged_attention_unroll.h | default | 3 | 952990 | -990830 | 2.014712 | 2.014712 | 4320 |

## Final

Runs per case: 5

| Case | Variant | Runs | elapsed_ns (median) | min-max ns | task_tp MTasks/s | subtask_tp MTasks/s | pool_high_water |
|------|---------|------|---------------------|------------|------------------|---------------------|-----------------|
| qwen3_dynamic_tensormap.h | tier=0 | 5 | 3871468 | -3902278 | 0.799697 | 0.799697 | 4242 |
| qwen3_dynamic_tensormap.h | tier=2 | 5 | 464624 | -471595 | 1.859568 | 6.663453 | 1290 |
| qwen3_dynamic_tensormap.h | tier=4 | 5 | 241742 | -260923 | 2.159327 | 12.807042 | 894 |
| paged_attention_unroll.h | default | 5 | 492434 | -540126 | 3.899000 | 3.899000 | 4320 |

## Summary (Baseline vs Final)

PTO2 对齐项：128B `TmEntry`、`tm_check_overlap` 三级 cascade（2D）、`TM_CLEANUP_INTERVAL=64` 周期 sync。

| Case | Variant | Baseline elapsed_ns | Final elapsed_ns | Delta | Baseline task_tp | Final task_tp |
|------|---------|--------------------:|-----------------:|------:|-----------------:|--------------:|
| qwen3_dynamic_tensormap.h | tier=0 | 4152020 | 3871468 | -6.7% | 0.746 | 0.800 |
| qwen3_dynamic_tensormap.h | tier=2 | 492685 | 464624 | -5.7% | 1.754 | 1.860 |
| qwen3_dynamic_tensormap.h | tier=4 | 199552 | 241742 | +21.1% | 2.616 | 2.159 |
| paged_attention_unroll.h | default | 952990 | 492434 | -48.3% | 2.015 | 3.899 |

微基准（`make test-tensormap`，scale=1，本机单次）：

| 路径 | ns/op |
|------|------:|
| insert (200k) | 11.52 |
| lookup (1M) | 43.00 |
| submit insert+sync (200k) | 29.64 |

正确性：`make test-tensormap`、`make test-dep-dump` 全绿；Qwen3 dep 边数 888（tier=4）不变。
