/*
 * AICore kernel thin entry — platform layer (ccec).
 */
#include "aicore_executor.h"
#include "kernel_args.h"
#include "swimlane_aicore.h"

#ifdef __CCE_KT_TEST__
#define __aicore__
#else
#define __aicore__ [aicore]
#endif

[[block_local]] static uint32_t s_aicore_profiling_flag;
[[block_local]] static __gm__ uint64_t *s_l2_swimlane_aicore_head_slot;
[[block_local]] static __gm__ L2SwimlaneActiveHead *s_l2_swimlane_aicore_head;

__attribute__((weak)) __aicore__ void set_aicore_profiling_flag(uint32_t flag)
{
    s_aicore_profiling_flag = flag;
}

__attribute__((weak)) __aicore__ uint32_t get_aicore_profiling_flag(void)
{
    return s_aicore_profiling_flag;
}

__attribute__((weak)) __aicore__ void set_l2_swimlane_aicore_head_slot(__gm__ uint64_t *slot_ptr)
{
    s_l2_swimlane_aicore_head_slot = slot_ptr;
    s_l2_swimlane_aicore_head = nullptr;
}

__attribute__((weak)) __aicore__ __gm__ L2SwimlaneActiveHead *get_l2_swimlane_aicore_head(void)
{
    if (s_l2_swimlane_aicore_head_slot == nullptr) {
        return nullptr;
    }
    if (s_l2_swimlane_aicore_head == nullptr) {
        dcci(s_l2_swimlane_aicore_head_slot, SINGLE_CACHE_LINE);
        s_l2_swimlane_aicore_head =
            reinterpret_cast<__gm__ L2SwimlaneActiveHead *>(*s_l2_swimlane_aicore_head_slot);
    }
    return s_l2_swimlane_aicore_head;
}

#ifdef __DAV_VEC__
#define KERNEL_ENTRY(x) x##_0_mix_aiv
#else
#define KERNEL_ENTRY(x) x##_0_mix_aic
#endif

#ifdef __DAV_VEC__
[[block_local]] int block_idx_aiv;
[[block_local]] CoreType core_type_aiv;
#else
[[block_local]] int block_idx_aic;
[[block_local]] CoreType core_type_aic;
#endif

extern "C" __global__ __aicore__ void KERNEL_ENTRY(aicore_kernel)(__gm__ KernelArgs *k_args)
{
#ifdef __DAV_VEC__
    block_idx_aiv = ESL_PROXY_ONBOARD_BLOCK_DIM + get_block_idx() * ESL_PROXY_AIV_LANES_PER_BLOCK +
                    get_subblockid();
    core_type_aiv = CoreType::AIV;
    set_ffts_base_addr((uint64_t)k_args->ffts_base_addr);
    ESL_SWIMLANE_AICORE_KERNEL_ENTRY(k_args, block_idx_aiv);
    aicore_execute(reinterpret_cast<__gm__ EslRuntime *>(k_args->runtime_args), block_idx_aiv, core_type_aiv,
                   k_args->enable_profiling_flag, k_args->l2_swimlane_aicore_rotation_table);
#else
    block_idx_aic = get_block_idx();
    core_type_aic = CoreType::AIC;
    set_ffts_base_addr((uint64_t)k_args->ffts_base_addr);
    ESL_SWIMLANE_AICORE_KERNEL_ENTRY(k_args, block_idx_aic);
    aicore_execute(reinterpret_cast<__gm__ EslRuntime *>(k_args->runtime_args), block_idx_aic, core_type_aic,
                   k_args->enable_profiling_flag, k_args->l2_swimlane_aicore_rotation_table);
#endif
}
