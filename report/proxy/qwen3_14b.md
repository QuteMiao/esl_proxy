# Qwen3_14b

## 理想性能
|#|任务耗时|X|吞吐 MTasks/s|时延 ns|
|:---:|:---:|:---:|:---:|:---:|
|1|5|240-4|48|250|
|2|10|240-4|24|500|
|3|5|192-2|38.4|250|
|4|10|192-2|19.2|500|


## Conf
spmd = 4

## Performance
```
make CASE=qwen3_dynamic_manual_scope.h run
make CASE=qwen3_dynamic_tensormap.h run
```
|tasks|subtasks|spmd|platform|
|:----:|:----:|:----:|:----:|
|864|3096|3.58|m5|

| Version | Duration/us | Task throughput MTasks/s | subtask throughput MTasks/s | predict_tasks MTasks/s | predict_subtasks MTasks/s |
|:----:|:----:|:----:|:----:|:----:|:----:|
| Orchestrator | 90 | 9.60 | 34.40 | / | / |
| O_Assign | 187 | 4.62 | 16.5 | / | / |
| O_Tensor  | 407 | 2.12 | 7.6 | 0.69 | 2.58 |
| O_Async | 142 | 6.08 | 21.8 | / | / |
| Scheduler | / | 14.9 | / | 4.85 | / |
| S_Async | / | 14.7 | 5.88 | / | / |

## Debug
```shell
make run CASE=qwen3_dynamic_manual_scope.h WORKER_LOG=1
python3 tools/gen_dag.py
```
## 关键问题
1. 如何解决性能不够的问题
2. 如何解决TensorMap的问题（Hash Table Size vs Cache Size）


## Apple M5
10 (4 Super and 6 Efficiency)，无SMT，采用多级动态频率调节（DVFS）机制

### 超大核 (Super Core) 
最高主频： 4.61 GHz
闲时频率：1.3 GHz
L1 Cache：192 KB 指令缓存 (L1i) + 128 KB 数据缓存 (L1d)
L2 Cache：4核共享 12～16 MB，3M/C
L3 / SLC：24 MB 的系统级缓存（System Level Cache）, 所有 CPU 、GPU 以及 Neural Engine 共享，6M/C

### 高能效核 (E-Core) 
最高主频： 3.05 GHz
闲时频率：972 MHz
L1 Cache：128 KB 指令缓存 (L1i) + 64 KB 数据缓存 (L1d)
L2 Cache：6核共享 6～8 MB，1M/C