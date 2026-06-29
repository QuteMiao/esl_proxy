/*
 * Onboard AICPU CPU 亲和性门控(对齐 simpler 的 platform_aicpu_affinity)。
 * host 探测 OCCUPY 掩码后,从 launch_count 个被拉起的控制线程里选出 allowed_count
 * 个钉到允许的 CPU 上运行,其余线程退出。防止 AICPU 控制线程漂移/争用拖垮握手。
 */
#ifndef ESL_PROXY_AICPU_AFFINITY_H
#define ESL_PROXY_AICPU_AFFINITY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* filter 式门控:所有 total_launched 个线程进入,各自读取 sched_getcpu() 上报,
 * 屏障同步后由一个 leader 线程做分类(把每个线程的 cpu 匹配到 allowed_cpus 的某个槽
 * → exec_idx,未匹配的按上报顺序补齐),随后每个线程返回是否存活。
 * 返回 1 表示本线程存活(*out_exec_idx 为角色 0..allowed_count-1,最后一个=orch),
 * 返回 0 表示本线程是多余线程,应直接退出。 */
int esl_aicpu_affinity_gate(const int32_t *allowed_cpus, int32_t allowed_count,
                            int32_t total_launched, int *out_exec_idx);

#ifdef __cplusplus
}
#endif

#endif /* ESL_PROXY_AICPU_AFFINITY_H */
