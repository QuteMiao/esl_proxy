/*
 * KernelArgs / EslKernelArgs — shared CANN kernel argument layout (Host, AICPU, AICore).
 */
#ifndef PLATFORM_COMMON_KERNEL_ARGS_H_
#define PLATFORM_COMMON_KERNEL_ARGS_H_

#include <stdint.h>

/* Forward declaration only — this platform header must not include the
 * algorithm-layer runtime.h. Only EslRuntime* is used below. */
typedef struct EslRuntime EslRuntime;

#if defined(__DAV_VEC__) || defined(__DAV_CUBE__)
#define __may_used_by_aicore__ __gm__
#else
#define __may_used_by_aicore__
#endif

typedef struct DeviceArgs DeviceArgs;
typedef EslRuntime Runtime;

#ifdef __cplusplus
extern "C" {
#endif

struct KernelArgs {
    uint64_t unused[5];
    DeviceArgs *device_args;
    __may_used_by_aicore__ EslRuntime *runtime_args;
    uint64_t regs;
    uint64_t ffts_base_addr;
    uint64_t dump_data_base;
    uint64_t _pad_reserved0;
    uint64_t _pad_reserved1;
    uint64_t dep_gen_data_base;
    uint64_t scope_stats_data_base;
    uint32_t log_level;
    uint32_t log_info_v;
    uint32_t _pad;
    uint64_t device_wall_data_base;
    uint32_t device_id;
};

typedef struct KernelArgs KernelArgs;
typedef struct KernelArgs EslKernelArgs;

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_COMMON_KERNEL_ARGS_H_ */
