/*
 * AICore kernel thin entry — platform layer (ccec).
 */
#include "aicore_executor.h"
#include "kernel_args.h"
#include "runtime.h"
#include "onboard_config.h"

#ifdef __CCE_KT_TEST__
#define __aicore__
#else
#define __aicore__ [aicore]
#endif

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
    aicore_execute(reinterpret_cast<__gm__ EslRuntime *>(k_args->runtime_args), block_idx_aiv, core_type_aiv);
#else
    block_idx_aic = get_block_idx();
    core_type_aic = CoreType::AIC;
    set_ffts_base_addr((uint64_t)k_args->ffts_base_addr);
    aicore_execute(reinterpret_cast<__gm__ EslRuntime *>(k_args->runtime_args), block_idx_aic, core_type_aic);
#endif
}
