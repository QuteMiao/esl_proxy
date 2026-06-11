# Qwen3_14b


## Conf
spmd = 4

## Performance
```
make CASE=qwen3_dynamic_manual_scope.h run
make CASE=qwen3_dynamic_tensormap.h run
```

| Item |O1_Base|O1_Assign|O1_Tensor|Scheduler|
|:----:|:----:|:----:|:----:|:----:|
|tasks| 864 | 
|subtasks| 3096|
|spmd| 3.58 |
|platform| M5 |
|duration/us| 90| 187 |407 | 60 |
|task throughput MTasks/s| 9.60 | 4.62 |2.12 | 14.9 |
|subtask throughput MTasks/s| 34.40 | 16.5 |7.6 | / |
|predict_tasks MTasks/s| / | / | 0.69 | 4.85 |
|predict_subtasks MTasks/s| / | / | 2.58 | / |

## Debug
```shell
make run CASE=qwen3_dynamic_manual_scope.h WORKER_LOG=1
python3 tools/gen_dag.py
```
## 关键问题
1. 如何解决多线程实时交互的问题


## Apple M5
10 (4 Super and 6 Efficiency)，无SMT，采用多级动态频率调节（DVFS）机制

### 超大核 (Super Core) 
最高主频： 4.61 GHz
闲时频率：1.3 GHz
L1 Cache：192 KB 指令缓存 (L1i) + 128 KB 数据缓存 (L1d)
L2 Cache：4 核共享 12～16 MB，3M/C
L3 / SLC：24 MB 的系统级缓存（System Level Cache）, 所有 CPU 、GPU 以及 Neural Engine 共享，6M/C

### 高能效核 (E-Core) 
最高主频： 3.05 GHz
闲时频率：972 MHz
L1 Cache：128 KB 指令缓存 (L1i) + 64 KB 数据缓存 (L1d)
L2 Cache：6核共享 6～8 MB，1M/C