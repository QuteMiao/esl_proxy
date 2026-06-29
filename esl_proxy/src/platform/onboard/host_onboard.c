/* host_onboard.c — host-side onboard bring-up (loader + launcher + AICore
 * register mapping + fingerprint). No main(): entry is src/main.c with
 * -DESL_PROXY_ONBOARD_HOST.
 */
#define _GNU_SOURCE

#include "dlog_pub.h"
#include "runtime.h"
#include "runtime.h"
#include "kernel_args.h"
#include "platform.h"
#include "onboard_config.h"
#include "swimlane_host.h"
#include "tools.h"

#include <acl/acl_rt.h>
#include <ascend_hal.h>
#include <dlfcn.h>
#include <errno.h>
#include <runtime/rt.h>
#include <runtime/runtime/rt_ffts.h>
#include <runtime/runtime/rts/rts_kernel.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ========================================================================== */
/* defines                                                                    */
/* ========================================================================== */

#ifndef ESL_PROXY_KARGS_LOG_LEVEL
#define ESL_PROXY_KARGS_LOG_LEVEL 1 /* DLOG_INFO */
#endif

#define kHalMemCtlEacces 13
#define kHalMemCtlMaxRetries 3
#define ESL_BOOTSTRAP_CACHE_MAX 32

#ifndef ESL_PROXY_CANN_AICPU_LAUNCH_THREADS
#define ESL_PROXY_CANN_AICPU_LAUNCH_THREADS 6
#endif

#define ACL_CHECK(call, msg)                                               \
    do {                                                                   \
        aclError _rc = (call);                                             \
        if (_rc != ACL_SUCCESS) {                                          \
            fprintf(stderr, "%s failed: %d (%s)\n", #call, (int)_rc, msg); \
            return 1;                                                      \
        }                                                                  \
    } while (0)

#define ESL_HOST_SYNC_STREAM(stream, label)                                \
    do {                                                                   \
        aclError _rc = aclrtSynchronizeStreamWithTimeout(                  \
            (stream), PLATFORM_STREAM_SYNC_TIMEOUT_MS);                    \
        if (_rc == ACL_ERROR_RT_STREAM_SYNC_TIMEOUT) {                     \
            fprintf(stderr,                                               \
                "[esl_proxy] %s: TIMEOUT after %d ms\n", (label),          \
                PLATFORM_STREAM_SYNC_TIMEOUT_MS);                          \
            return 1;                                                      \
        }                                                                  \
        if (_rc != ACL_SUCCESS) {                                          \
            fprintf(stderr, "[esl_proxy] %s failed: %d\n", (label),        \
                (int)_rc);                                                 \
            return 1;                                                      \
        }                                                                  \
    } while (0)

#define ESL_HOST_SYNC_STREAM_DUMP(stream, label, dev_wall_ptr)             \
    do {                                                                   \
        aclError _rc = aclrtSynchronizeStreamWithTimeout(                  \
            (stream), PLATFORM_STREAM_SYNC_TIMEOUT_MS);                    \
        if (_rc == ACL_ERROR_RT_STREAM_SYNC_TIMEOUT) {                     \
            fprintf(stderr,                                               \
                "[esl_proxy] %s: TIMEOUT after %d ms\n", (label),          \
                PLATFORM_STREAM_SYNC_TIMEOUT_MS);                          \
            esl_host_dump_device_wall((dev_wall_ptr));                     \
            return 1;                                                      \
        }                                                                  \
        if (_rc != ACL_SUCCESS) {                                          \
            fprintf(stderr, "[esl_proxy] %s failed: %d\n", (label),        \
                (int)_rc);                                                 \
            esl_host_dump_device_wall((dev_wall_ptr));                     \
            return 1;                                                      \
        }                                                                  \
    } while (0)

#define AICORE_CORE_STRIDE (8ULL * 1024ULL * 1024ULL)
#define AICORE_SUB_CORE_STRIDE 0x100000ULL
#define AICORE_REG_VALID_FALLBACK 0xFFFFFFFFULL
#define ESL_AICPU_JSON_PATH_FMT "/tmp/esl_proxy_inner_%016lx_%d.json"
#define ESL_INNER_OP_TYPE_FMT "%s_%016lx"

/* ========================================================================== */
/* types & structs                                                            */
/* ========================================================================== */

typedef int (*HalGetDeviceInfoByBuffFn)(uint64_t deviceId, int32_t moduleType,
    int32_t infoType, void *buf, int32_t *size);
typedef int (*HalMemCtlFn)(int type, void *paramValue, size_t paramValueSize,
    void *outValue, size_t *outSizeRet);

typedef struct EslAicpuLoader {
    void *binary_handle;
    rtFuncHandle init_handle;
    rtFuncHandle exec_handle;
    char json_path[256];
    uint64_t inner_fp;
    int device_id;
    char inner_basename[64];
} EslAicpuLoader;

typedef struct {
    uint64_t fp;
    int device_id;
} BootstrapKey;

typedef struct {
    void *ptr;
} DevBuf;

typedef struct EslAicoreLauncher {
    char *binary;
    size_t binary_len;
    void *bin_handle;
} EslAicoreLauncher;

typedef struct {
    void *ptr;
} DevMem;

typedef struct {
    struct {
        uint64_t unused[5];
        uint64_t device_args_ptr;
        uint64_t pad[20];
    } k_args;
    char kernel_name[32];
    char so_name[32];
    char op_name[32];
} EslBootstrapLaunchArgs;

typedef struct {
    EslKernelArgs *k_args;
} EslAicoreLaunchArgs;

/* ========================================================================== */
/* static globals                                                             */
/* ========================================================================== */

static BootstrapKey g_bootstrapped[ESL_BOOTSTRAP_CACHE_MAX];
static size_t g_bootstrapped_count;

/* ========================================================================== */
/* AICore register mapping                                                    */
/* ========================================================================== */

/* 通过 HAL 查询设备上 AICore 核心的占用位图掩码。 */
static int get_pg_mask(uint64_t *valid, uint64_t device_id)
{
    uint64_t aicore_bitmap[PLATFORM_AICORE_MAP_BUFF_LEN] = {0};
    int32_t size_n = (int32_t)(sizeof(uint64_t) * PLATFORM_AICORE_MAP_BUFF_LEN);
    HalGetDeviceInfoByBuffFn halFuncDevInfo;

    halFuncDevInfo =
        (HalGetDeviceInfoByBuffFn)dlsym(RTLD_DEFAULT, "halGetDeviceInfoByBuff");
    if (halFuncDevInfo == NULL) {
        return 0;
    }

    if (halFuncDevInfo(device_id, MODULE_TYPE_AICORE, INFO_TYPE_OCCUPY,
            aicore_bitmap, &size_n) != 0) {
        return 0;
    }
    *valid = aicore_bitmap[0];
    return 1;
}

/* 枚举设备上所有 AIC/AIV 核心的寄存器虚拟地址。 */
static int get_aicore_reg_info(int64_t **aic, size_t *aic_len, size_t *aic_cap,
    int64_t **aiv, size_t *aiv_len, size_t *aiv_cap,
    int addr_type, int64_t device_id)
{
    uint64_t valid = 0;
    HalMemCtlFn halFunc;
    struct AddrMapInPara in_map_para;
    struct AddrMapOutPara out_map_para;
    int ret;
    int attempt;
    uint32_t i;
    uint32_t j;

    if (!get_pg_mask(&valid, (uint64_t)device_id)) {
        valid = 0xFFFFFFFFULL;
    }

    halFunc = (HalMemCtlFn)dlsym(RTLD_DEFAULT, "halMemCtl");
    if (halFunc == NULL) {
        fprintf(stderr, "halMemCtl not found\n");
        return -1;
    }

    memset(&in_map_para, 0, sizeof(in_map_para));
    memset(&out_map_para, 0, sizeof(out_map_para));
    in_map_para.devid = device_id;
    in_map_para.addr_type = addr_type;

    ret = -1;
    for (attempt = 0; attempt <= kHalMemCtlMaxRetries; ++attempt) {
        ret = halFunc(0, &in_map_para, sizeof(in_map_para), &out_map_para, NULL);
        if (ret != kHalMemCtlEacces) {
            break;
        }
    }
    if (ret != 0) {
        fprintf(stderr, "halMemCtl failed: %d\n", ret);
        return ret;
    }

    for (i = 0; i < DAV_2201_PLATFORM_MAX_PHYSICAL_CORES; i++) {
        for (j = 0; j < PLATFORM_SUB_CORES_PER_AICORE; j++) {
            uint64_t vaddr = 0;

            if ((valid & (1ULL << i)) != 0) {
                vaddr = out_map_para.ptr +
                        (i * AICORE_CORE_STRIDE + j * AICORE_SUB_CORE_STRIDE);
            }
            if (j == 0) {
                if (grow_array(aic, aic_cap, aic_len, (int64_t)vaddr) == NULL) {
                    return -1;
                }
            } else {
                if (grow_array(aiv, aiv_cap, aiv_len, (int64_t)vaddr) == NULL) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* 将 AICore 寄存器地址表拷贝到设备 GM 并返回设备指针。 */
int esl_host_init_aicore_regs(uint64_t device_id, uint64_t *out_dev_regs)
{
    int64_t *aic = NULL;
    size_t aic_len = 0;
    size_t aic_cap = 0;
    int64_t *aiv = NULL;
    size_t aiv_len = 0;
    size_t aiv_cap = 0;
    int64_t *host_regs = NULL;
    size_t host_regs_len = 0;
    size_t host_regs_cap = 0;
    size_t i;
    size_t regs_size;
    void *dev_ptr;
    aclError ac;
    int rc;

    if (out_dev_regs == NULL) {
        return -1;
    }
    *out_dev_regs = 0;

    rc = get_aicore_reg_info(&aic, &aic_len, &aic_cap, &aiv, &aiv_len, &aiv_cap,
        ADDR_MAP_TYPE_REG_AIC_CTRL, (int64_t)device_id);
    if (rc != 0) {
        free(aic);
        free(aiv);
        return rc;
    }

    for (i = 0; i < aic_len; ++i) {
        if (grow_array(&host_regs, &host_regs_cap, &host_regs_len, aic[i]) ==
            NULL) {
            free(aic);
            free(aiv);
            free(host_regs);
            return -1;
        }
    }
    for (i = 0; i < aiv_len; ++i) {
        if (grow_array(&host_regs, &host_regs_cap, &host_regs_len, aiv[i]) ==
            NULL) {
            free(aic);
            free(aiv);
            free(host_regs);
            return -1;
        }
    }

    free(aic);
    free(aiv);

    if (host_regs_len == 0) {
        free(host_regs);
        return -1;
    }

    regs_size = host_regs_len * sizeof(int64_t);
    dev_ptr = NULL;
    ac = aclrtMalloc(&dev_ptr, regs_size, ACL_MEM_MALLOC_HUGE_FIRST);
    if (ac != ACL_SUCCESS) {
        free(host_regs);
        return (int)ac;
    }
    ac = aclrtMemcpy(dev_ptr, regs_size, host_regs, regs_size,
        ACL_MEMCPY_HOST_TO_DEVICE);
    free(host_regs);
    if (ac != ACL_SUCCESS) {
        aclrtFree(dev_ptr);
        return (int)ac;
    }

    *out_dev_regs = (uint64_t)(uintptr_t)dev_ptr;
    fprintf(stderr, "[esl_proxy] HW AICore regs: %zu addresses H2D -> 0x%lx\n",
        host_regs_len, (unsigned long)*out_dev_regs);
    return 0;
}

/* ========================================================================== */
/* AICPU loader                                                               */
/* ========================================================================== */

/* 在设备上分配 DevBuf 内存。 */
static aclError devbuf_alloc(DevBuf *buf, size_t n)
{
    return aclrtMalloc(&buf->ptr, n, ACL_MEM_MALLOC_HUGE_FIRST);
}

/* 释放 DevBuf 设备内存。 */
static void devbuf_free(DevBuf *buf)
{
    if (buf->ptr != NULL) {
        aclrtFree(buf->ptr);
        buf->ptr = NULL;
    }
}

/* 检查指定指纹与设备是否已完成 AICPU inner SO 引导。 */
static int bootstrap_seen(uint64_t fp, int device_id)
{
    size_t i;

    for (i = 0; i < g_bootstrapped_count; ++i) {
        if (g_bootstrapped[i].fp == fp &&
            g_bootstrapped[i].device_id == device_id) {
            return 1;
        }
    }
    return 0;
}

/* 记录已完成的 AICPU inner SO 引导指纹。 */
static int bootstrap_insert(uint64_t fp, int device_id)
{
    if (g_bootstrapped_count >= ESL_BOOTSTRAP_CACHE_MAX) {
        return 0;
    }
    g_bootstrapped[g_bootstrapped_count].fp = fp;
    g_bootstrapped[g_bootstrapped_count].device_id = device_id;
    g_bootstrapped_count++;
    return 1;
}

/* 创建 AICPU 加载器实例。 */
EslAicpuLoader *esl_aicpu_loader_create(void)
{
    return (EslAicpuLoader *)calloc(1, sizeof(EslAicpuLoader));
}

/* 销毁 AICPU 加载器并清理临时 JSON 与已加载二进制。 */
void esl_aicpu_loader_destroy(EslAicpuLoader *loader)
{
    if (loader == NULL) {
        return;
    }
    if (loader->binary_handle != NULL) {
        rtsBinaryUnload(loader->binary_handle);
    }
    if (loader->json_path[0] != '\0') {
        remove(loader->json_path);
    }
    free(loader);
}

/* 通过 KFC 内核将 dispatcher/inner SO 引导到设备。 */
int esl_aicpu_loader_bootstrap(EslAicpuLoader *loader,
    const void *dispatcher_so, size_t dispatcher_len,
    const void *inner_so, size_t inner_len,
    rtStream_t stream, int device_id)
{
    DevBuf dev_dispatcher = {0};
    DevBuf dev_inner = {0};
    DevBuf dev_args = {0};
    aclError rc;
    char host_dev_args[160];
    EslBootstrapLaunchArgs args;
    rtAicpuArgsEx_t rt_args;
    rtError_t rrc;

    if (loader == NULL || dispatcher_so == NULL || dispatcher_len == 0 ||
        inner_so == NULL || inner_len == 0) {
        return -1;
    }

    loader->device_id = device_id;
    loader->inner_fp = esl_fingerprint_bytes(inner_so, inner_len);
    esl_make_inner_basename(loader->inner_fp, device_id, loader->inner_basename,
        sizeof(loader->inner_basename));

    if (bootstrap_seen(loader->inner_fp, device_id)) {
        fprintf(
            stderr,
            "[esl_proxy] AICPU inner SO already bootstrapped fp=%016lx dev=%d\n",
            (unsigned long)loader->inner_fp, device_id);
        return 0;
    }

    rc = devbuf_alloc(&dev_dispatcher, dispatcher_len);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }
    rc = aclrtMemcpy(dev_dispatcher.ptr, dispatcher_len, dispatcher_so,
        dispatcher_len, ACL_MEMCPY_HOST_TO_DEVICE);
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }
    rc = devbuf_alloc(&dev_inner, inner_len);
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }
    rc = aclrtMemcpy(dev_inner.ptr, inner_len, inner_so, inner_len,
        ACL_MEMCPY_HOST_TO_DEVICE);
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_inner);
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }

    memset(host_dev_args, 0, sizeof(host_dev_args));
    {
        uint64_t v;

        v = (uint64_t)(uintptr_t)dev_dispatcher.ptr;
        memcpy(host_dev_args + 96, &v, sizeof(v));
        v = (uint64_t)dispatcher_len;
        memcpy(host_dev_args + 104, &v, sizeof(v));
        v = (uint64_t)(unsigned)device_id;
        memcpy(host_dev_args + 112, &v, sizeof(v));
        v = (uint64_t)(uintptr_t)dev_inner.ptr;
        memcpy(host_dev_args + 120, &v, sizeof(v));
        v = (uint64_t)inner_len;
        memcpy(host_dev_args + 128, &v, sizeof(v));
    }

    rc = devbuf_alloc(&dev_args, sizeof(host_dev_args));
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_inner);
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }
    rc = aclrtMemcpy(dev_args.ptr, sizeof(host_dev_args), host_dev_args,
        sizeof(host_dev_args), ACL_MEMCPY_HOST_TO_DEVICE);
    if (rc != ACL_SUCCESS) {
        devbuf_free(&dev_args);
        devbuf_free(&dev_inner);
        devbuf_free(&dev_dispatcher);
        return (int)rc;
    }

    memset(&args, 0, sizeof(args));
    args.k_args.device_args_ptr = (uint64_t)(uintptr_t)dev_args.ptr;
    strncpy(args.kernel_name, "DynTileFwkKernelServerInit",
        sizeof(args.kernel_name) - 1);
    strncpy(args.so_name, "libaicpu_extend_kernels.so", sizeof(args.so_name) - 1);

    memset(&rt_args, 0, sizeof(rt_args));
    rt_args.args = &args;
    rt_args.argsSize = sizeof(args);
    rt_args.kernelNameAddrOffset = offsetof(EslBootstrapLaunchArgs, kernel_name);
    rt_args.soNameAddrOffset = offsetof(EslBootstrapLaunchArgs, so_name);

    rrc = rtAicpuKernelLaunchExWithArgs(KERNEL_TYPE_AICPU_KFC, "AST_DYN_AICPU", 1,
        &rt_args, NULL, stream, 0);
    if (rrc != RT_ERROR_NONE) {
        fprintf(stderr, "rtAicpuKernelLaunchExWithArgs bootstrap failed: %d\n",
            (int)rrc);
        devbuf_free(&dev_args);
        devbuf_free(&dev_inner);
        devbuf_free(&dev_dispatcher);
        return (int)rrc;
    }
    rc = aclrtSynchronizeStreamWithTimeout(stream, PLATFORM_STREAM_SYNC_TIMEOUT_MS);
    devbuf_free(&dev_args);
    devbuf_free(&dev_inner);
    devbuf_free(&dev_dispatcher);
    if (rc != ACL_SUCCESS) {
        return (int)rc;
    }

    bootstrap_insert(loader->inner_fp, device_id);
    fprintf(stderr, "[esl_proxy] bootstrapped inner SO %s\n",
        loader->inner_basename);
    return 0;
}

/* 加载 AICPU JSON 描述并解析 init/exec 符号句柄。 */
int esl_aicpu_loader_init(EslAicpuLoader *loader)
{
    char init_op[128];
    char exec_op[128];
    rtLoadBinaryOption_t option;
    rtLoadBinaryConfig_t load_config;
    rtError_t rc;

    if (loader == NULL || loader->inner_fp == 0) {
        return -1;
    }

    snprintf(loader->json_path, sizeof(loader->json_path),
        "/tmp/esl_proxy_inner_%016lx_%d.json",
        (unsigned long)loader->inner_fp, getpid());
    if (!esl_write_aicpu_kernel_json(loader->json_path, loader->inner_basename,
            loader->inner_fp)) {
        return -1;
    }

    memset(&option, 0, sizeof(option));
    option.optionId = RT_LOAD_BINARY_OPT_CPU_KERNEL_MODE;
    option.value.cpuKernelMode = 0;
    memset(&load_config, 0, sizeof(load_config));
    load_config.options = &option;
    load_config.numOpt = 1;

    rc = rtsBinaryLoadFromFile(loader->json_path, &load_config,
        &loader->binary_handle);
    if (rc != RT_ERROR_NONE) {
        fprintf(stderr, "rtsBinaryLoadFromFile failed: %d\n", (int)rc);
        return (int)rc;
    }

    esl_make_aicpu_op_type(ESL_AICPU_INIT_NAME, loader->inner_fp, init_op,
        sizeof(init_op));
    esl_make_aicpu_op_type(ESL_AICPU_EXEC_NAME, loader->inner_fp, exec_op,
        sizeof(exec_op));
    rc = rtsFuncGetByName(loader->binary_handle, init_op, &loader->init_handle);
    if (rc != RT_ERROR_NONE) {
        return (int)rc;
    }
    rc = rtsFuncGetByName(loader->binary_handle, exec_op, &loader->exec_handle);
    if (rc != RT_ERROR_NONE) {
        return (int)rc;
    }
    return 0;
}

/* 在指定 stream 上启动 AICPU init 或 exec 内核。 */
int esl_aicpu_loader_launch(EslAicpuLoader *loader, rtStream_t stream,
    EslKernelArgs *k_args, int aicpu_num, const char *symbol_name)
{
    rtFuncHandle handle;
    rtCpuKernelArgs_t cpu_args;
    rtLaunchKernelAttr_t attr;
    rtKernelLaunchCfg_t cfg;
    rtError_t rc;

    if (loader == NULL || k_args == NULL || symbol_name == NULL) {
        return -1;
    }

    if (strcmp(symbol_name, ESL_AICPU_INIT_NAME) == 0) {
        handle = loader->init_handle;
    } else if (strcmp(symbol_name, ESL_AICPU_EXEC_NAME) == 0) {
        handle = loader->exec_handle;
    } else {
        return -1;
    }

    memset(&cpu_args, 0, sizeof(cpu_args));
    cpu_args.baseArgs.args = k_args;
    cpu_args.baseArgs.argsSize = sizeof(EslKernelArgs);
    memset(&attr, 0, sizeof(attr));
    cfg.attrs = &attr;
    cfg.numAttrs = 0U;

    rc = rtsLaunchCpuKernel(handle, (uint32_t)aicpu_num, stream, &cfg, &cpu_args);
    if (rc != RT_ERROR_NONE) {
        fprintf(stderr, "rtsLaunchCpuKernel(%s) failed: %d\n", symbol_name,
            (int)rc);
        return (int)rc;
    }
    return 0;
}

/* ========================================================================== */
/* AICore launcher                                                            */
/* ========================================================================== */

/* 创建 AICore 启动器实例。 */
EslAicoreLauncher *esl_aicore_launcher_create(void)
{
    return (EslAicoreLauncher *)calloc(1, sizeof(EslAicoreLauncher));
}

/* 销毁 AICore 启动器并释放 ELF 缓冲。 */
void esl_aicore_launcher_destroy(EslAicoreLauncher *launcher)
{
    if (launcher == NULL) {
        return;
    }
    free(launcher->binary);
    free(launcher);
}

/* 将 AICore ELF 二进制拷贝到主机内存以备注册。 */
int esl_aicore_launcher_load(EslAicoreLauncher *launcher, const void *elf_data,
    size_t elf_len)
{
    char *copy;

    if (launcher == NULL || elf_data == NULL || elf_len == 0) {
        return -1;
    }
    copy = (char *)malloc(elf_len);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, elf_data, elf_len);
    free(launcher->binary);
    launcher->binary = copy;
    launcher->binary_len = elf_len;
    launcher->bin_handle = NULL;
    return 0;
}

static int esl_aicore_launcher_register(EslAicoreLauncher *launcher)
{
    rtDevBinary_t binary;
    rtError_t rc;

    if (launcher == NULL) {
        return -1;
    }
    if (launcher->bin_handle != NULL) {
        return 0;
    }
    memset(&binary, 0, sizeof(binary));
    binary.magic = RT_DEV_BINARY_MAGIC_ELF;
    binary.version = 0;
    binary.data = launcher->binary;
    binary.length = launcher->binary_len;
    rc = rtRegisterAllKernel(&binary, &launcher->bin_handle);
    if (rc != RT_ERROR_NONE) {
        fprintf(stderr, "rtRegisterAllKernel failed: %d\n", (int)rc);
        return (int)rc;
    }
    return 0;
}

/* 注册并启动 AICore 内核（按 block_dim 下发）。 */
int esl_aicore_launcher_launch(EslAicoreLauncher *launcher, rtStream_t stream,
    void *k_args_dev, int block_dim)
{
    rtArgsEx_t rt_args;
    rtTaskCfgInfo_t cfg;
    rtError_t rc;
    EslAicoreLaunchArgs args;

    if (launcher == NULL || k_args_dev == NULL || block_dim < 1) {
        return -1;
    }

    if (esl_aicore_launcher_register(launcher) != 0) {
        return -1;
    }

    args.k_args = (EslKernelArgs *)k_args_dev;
    memset(&rt_args, 0, sizeof(rt_args));
    rt_args.args = &args;
    rt_args.argsSize = sizeof(args);

    memset(&cfg, 0, sizeof(cfg));
    cfg.schemMode = RT_SCHEM_MODE_BATCH;

    rc = rtKernelLaunchWithHandleV2(launcher->bin_handle, 0, (uint32_t)block_dim,
        &rt_args, NULL, stream, &cfg);
    if (rc != RT_ERROR_NONE) {
        fprintf(stderr, "rtKernelLaunchWithHandleV2 failed: %d\n", (int)rc);
        return (int)rc;
    }
    return 0;
}

/* ========================================================================== */
/* host runner                                                                */
/* ========================================================================== */

/* 释放 DevMem 设备内存。 */
static void devmem_free(DevMem *mem)
{
    if (mem->ptr != NULL) {
        aclrtFree(mem->ptr);
        mem->ptr = NULL;
    }
}

/* 在设备上分配 DevMem 内存。 */
static aclError devmem_alloc(DevMem *mem, size_t n)
{
    return aclrtMalloc(&mem->ptr, n, ACL_MEM_MALLOC_HUGE_FIRST);
}

/* 在未设置 ASCEND_GLOBAL_LOG_LEVEL 时同步 CANN 日志级别。 */
static void esl_host_sync_cann_log_level(int level)
{
    if (getenv("ASCEND_GLOBAL_LOG_LEVEL") != NULL) {
        return;
    }
    if (level < DLOG_DEBUG) {
        level = DLOG_DEBUG;
    }
    if (level > DLOG_ERROR) {
        level = DLOG_ERROR;
    }
    dlog_setlevel(-1, level, 0);
}

/* 解析实际使用的 NPU 设备号（优先 TASK_DEVICE 环境变量）。 */
static int resolve_device_id(int cli_dev)
{
    const char *env = getenv("TASK_DEVICE");

    if (env != NULL && env[0] != '\0') {
        return atoi(env);
    }
    return cli_dev;
}

/* 打印命令行用法说明。 */
static void usage(const char *prog)
{
    fprintf(
        stderr,
        "usage: %s [-d device_id] [--dispatcher PATH] [--aicpu PATH] [--aicore "
        "PATH]\n"
        "  defaults:\n"
        "    dispatcher = build/onboard/aicpu/libesl_aicpu_dispatcher.so\n"
        "    aicpu      = build/onboard/aicpu/libaicpu_kernel.so\n"
        "    aicore     = build/onboard/aicore/aicore_kernel.o\n",
        prog);
}

/* halGetDeviceInfo(deviceId, moduleType, infoType, &value):用 OCCUPY 选择子查询
 * AICPU 用户可调度核掩码。沿用 simpler 的 CANN 驱动 ABI 选择子。 */
typedef int (*HalGetDeviceInfoFn)(uint64_t deviceId, int32_t moduleType,
    int32_t infoType, int64_t *value);

#define ESL_HAL_MODULE_AICPU 1
#define ESL_HAL_INFO_OCCUPY 8
/* a2a3 AICPU 无 SMT:每个逻辑 cpu_id 1:1 映射物理核,cluster 可由 cpu_id 算得。
 * 8 核/die,4 核/cluster ⇒ 2 cluster/die。 */
#define ESL_AICPU_CORES_PER_DIE 8
#define ESL_AICPU_CPUS_PER_CLUSTER 4

/* dlsym halGetDeviceInfo:先 RTLD_DEFAULT,再 dlopen libascend_hal.so 的两个候选路径。 */
static HalGetDeviceInfoFn esl_load_hal_get_device_info(void)
{
    HalGetDeviceInfoFn fn;
    size_t i;
    static const char *const kHalLibs[] = {
        "libascend_hal.so",
        "/usr/local/Ascend/driver/lib64/driver/libascend_hal.so",
    };

    fn = (HalGetDeviceInfoFn)dlsym(RTLD_DEFAULT, "halGetDeviceInfo");
    if (fn != NULL) {
        return fn;
    }
    for (i = 0; i < sizeof(kHalLibs) / sizeof(kHalLibs[0]); ++i) {
        if (dlopen(kHalLibs[i], RTLD_LAZY | RTLD_GLOBAL) == NULL) {
            continue;
        }
        fn = (HalGetDeviceInfoFn)dlsym(RTLD_DEFAULT, "halGetDeviceInfo");
        if (fn != NULL) {
            return fn;
        }
    }
    return NULL;
}

/* 探测设备的 AICPU OCCUPY 掩码,挑出 active_count 个允许的控制核(优先单 cluster:从最高
 * cluster 起选第一个核数 >= active_count 的 cluster,按 cpu_id 升序;否则跨 cluster 取最小
 * cpu_id),写入 rt->aicpu_allowed_cpus[] 等字段。任何失败回退到老行为(门控空转)。 */
static void esl_host_probe_aicpu_allowed(int device_id, EslRuntime *rt, int active_count)
{
    HalGetDeviceInfoFn fn;
    int64_t v = 0;
    uint64_t occupy;
    int32_t user_cpus[64];
    int32_t user_cluster[64];
    int32_t num_user;
    int32_t max_cluster;
    int32_t cpu_id;
    int32_t chosen_cluster;
    int32_t cluster;
    int filled;

    rt->aicpu_allowed_cpu_count = 0;
    rt->aicpu_launch_count = active_count;

    if (rt == NULL || active_count <= 0 || active_count > 16) {
        fprintf(stderr, "[esl_proxy] AICPU allowed_cpus=[] launch=%d (bad active_count)\n",
            rt ? rt->aicpu_launch_count : 0);
        return;
    }

    fn = esl_load_hal_get_device_info();
    if (fn == NULL) {
        fprintf(stderr, "[esl_proxy] AICPU allowed_cpus=[] launch=%d (no halGetDeviceInfo)\n",
            active_count);
        return;
    }
    if (fn((uint64_t)device_id, ESL_HAL_MODULE_AICPU, ESL_HAL_INFO_OCCUPY, &v) != 0) {
        fprintf(stderr, "[esl_proxy] AICPU allowed_cpus=[] launch=%d (OCCUPY query failed)\n",
            active_count);
        return;
    }
    occupy = (uint64_t)v;

    num_user = 0;
    max_cluster = -1;
    for (cpu_id = 0; cpu_id < 64; ++cpu_id) {
        int32_t cl;
        if (((occupy >> cpu_id) & 1ULL) == 0) {
            continue;
        }
        cl = (cpu_id % ESL_AICPU_CORES_PER_DIE) / ESL_AICPU_CPUS_PER_CLUSTER;
        user_cpus[num_user] = cpu_id;
        user_cluster[num_user] = cl;
        if (cl > max_cluster) {
            max_cluster = cl;
        }
        ++num_user;
    }

    if (num_user < active_count || max_cluster < 0) {
        fprintf(stderr,
            "[esl_proxy] AICPU allowed_cpus=[] launch=%d (occupy=0x%llx user_cpus=%d < active=%d)\n",
            active_count, (unsigned long long)occupy, num_user, active_count);
        return;
    }

    /* 优先选一个能容纳全部 active 线程的 cluster(从高到低)。cpu_id 在采集时已升序。 */
    chosen_cluster = -1;
    for (cluster = max_cluster; cluster >= 0; --cluster) {
        int32_t cnt = 0;
        int32_t k;
        for (k = 0; k < num_user; ++k) {
            if (user_cluster[k] == cluster) {
                ++cnt;
            }
        }
        if (cnt >= active_count) {
            chosen_cluster = cluster;
            break;
        }
    }

    filled = 0;
    if (chosen_cluster >= 0) {
        int32_t k;
        for (k = 0; k < num_user && filled < active_count; ++k) {
            if (user_cluster[k] == chosen_cluster) {
                rt->aicpu_allowed_cpus[filled++] = user_cpus[k];
            }
        }
    } else {
        /* 没有单 cluster 能装下 → 跨 cluster 取最小 cpu_id(确定性回退,可能跨 NUMA)。 */
        int32_t k;
        fprintf(stderr, "[esl_proxy] AICPU: no single cluster holds %d threads; cross-cluster fallback\n",
            active_count);
        for (k = 0; k < num_user && filled < active_count; ++k) {
            rt->aicpu_allowed_cpus[filled++] = user_cpus[k];
        }
    }

    if (filled < active_count) {
        rt->aicpu_allowed_cpu_count = 0;
        rt->aicpu_launch_count = active_count;
        fprintf(stderr, "[esl_proxy] AICPU allowed_cpus=[] launch=%d (could not fit)\n", active_count);
        return;
    }

    rt->aicpu_allowed_cpu_count = active_count;
    /* launch 略多于 allowed,让 CANN 在 OCCUPY 池里铺开线程,门控再钉出 allowed 个。 */
    rt->aicpu_launch_count = (num_user < 16) ? num_user : 16;

    {
        char buf[256];
        int off = 0;
        int k;
        for (k = 0; k < active_count; ++k) {
            int n = snprintf(buf + off, sizeof(buf) - (size_t)off, "%s%d",
                (k == 0) ? "" : ",", rt->aicpu_allowed_cpus[k]);
            if (n < 0 || (size_t)(off + n) >= sizeof(buf)) {
                break;
            }
            off += n;
        }
        fprintf(stderr, "[esl_proxy] AICPU allowed_cpus=[%s] launch=%d\n", buf,
            rt->aicpu_launch_count);
    }
}

/* 上板主流程：加载 SO、初始化寄存器与 GM、启动 AICPU/AICore 并校验结果。 */
int esl_onboard_run(int argc, char **argv)
{
    int device_id = 0;
    char dispatcher_path[512] = {0};
    char aicpu_path[512] = {0};
    char aicore_path[512] = {0};
    char *dispatcher_bytes = NULL;
    char *aicpu_bytes = NULL;
    char *aicore_bytes = NULL;
    size_t dispatcher_len = 0;
    size_t aicpu_len = 0;
    size_t aicore_len = 0;
    rtStream_t stream_aicpu = NULL;
    rtStream_t stream_aicore = NULL;
    EslAicpuLoader *loader;
    EslAicoreLauncher *aicore;
    uint64_t dev_regs = 0;
    EslRuntime host_runtime;
    DevMem dev_runtime = {0};
    DevMem dev_wall = {0};
    DevMem dev_k_args = {0};
    DevMem dev_payload = {0};
    EslKernelArgs k_args;
    size_t payload_bytes;
    int i;
    int rc;
    uint64_t stats[8];
    aclError stats_rc;
    const char *env;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            device_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dispatcher") == 0 && i + 1 < argc) {
            strncpy(dispatcher_path, argv[++i], sizeof(dispatcher_path) - 1);
        } else if (strcmp(argv[i], "--aicpu") == 0 && i + 1 < argc) {
            strncpy(aicpu_path, argv[++i], sizeof(aicpu_path) - 1);
        } else if (strcmp(argv[i], "--aicore") == 0 && i + 1 < argc) {
            strncpy(aicore_path, argv[++i], sizeof(aicore_path) - 1);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    device_id = resolve_device_id(device_id);

    if (dispatcher_path[0] == '\0') {
        env = getenv("ESL_PROXY_DISPATCHER_SO");
        strncpy(dispatcher_path,
            env ? env : "build/onboard/aicpu/libesl_aicpu_dispatcher.so",
            sizeof(dispatcher_path) - 1);
    }
    if (aicpu_path[0] == '\0') {
        env = getenv("ESL_PROXY_AICPU_SO");
        strncpy(aicpu_path, env ? env : "build/onboard/aicpu/libaicpu_kernel.so",
            sizeof(aicpu_path) - 1);
    }
    if (aicore_path[0] == '\0') {
        env = getenv("ESL_PROXY_AICORE_ELF");
        strncpy(aicore_path, env ? env : "build/onboard/aicore/aicore_kernel.o",
            sizeof(aicore_path) - 1);
    }

    if (!read_file(dispatcher_path, &dispatcher_bytes, &dispatcher_len)) {
        return 1;
    }
    if (!read_file(aicpu_path, &aicpu_bytes, &aicpu_len)) {
        free(dispatcher_bytes);
        return 1;
    }
    if (!read_file(aicore_path, &aicore_bytes, &aicore_len)) {
        free(dispatcher_bytes);
        free(aicpu_bytes);
        return 1;
    }

    fprintf(stderr,
        "[esl_proxy] onboard runner device=%d block_dim=%d "
        "workers=%d(aic=%d+aiv=%d) aicpu_threads=%d launch_threads=%d\n",
        device_id, ESL_PROXY_ONBOARD_BLOCK_DIM,
        ESL_PROXY_ONBOARD_WORKER_COUNT, ESL_PROXY_ONBOARD_AIC_COUNT,
        ESL_PROXY_ONBOARD_AIV_COUNT, ESL_PROXY_AICPU_THREAD_NUM,
        ESL_PROXY_CANN_AICPU_LAUNCH_THREADS);
    fprintf(stderr, "[esl_proxy] dispatcher=%s aicpu=%s aicore=%s\n",
        dispatcher_path, aicpu_path, aicore_path);

    ACL_CHECK(aclInit(NULL), "aclInit");
    esl_host_sync_cann_log_level(ESL_PROXY_KARGS_LOG_LEVEL);
    ACL_CHECK(aclrtSetDevice(device_id), "aclrtSetDevice");
    /* 防御性复位:上一个 run 若以 507018 退出,常驻 AICore kernel 没收到 EXIT、设备被"毒化"
     * (aclrtSetDevice 清不掉,实测同一卡上连续进程 ready_cnt 仍 3/69 交替)。开局先 reset 一次,
     * 把残留的常驻 AICore 清干净,保证每次从干净设备起步。 */
    {
        aclError _reset_rc = aclrtResetDevice(device_id);
        if (_reset_rc != ACL_SUCCESS) {
            fprintf(stderr, "[esl_proxy] startup aclrtResetDevice(%d) rc=%d (continuing)\n",
                    device_id, (int)_reset_rc);
        }
        ACL_CHECK(aclrtSetDevice(device_id), "aclrtSetDevice after reset");
    }
    /* 关键(对齐 simpler):AICPU/AICore 内核是常驻 kernel(整轮 busy-wait),
     * 必须把 STARS 的 op-execute 看门狗放宽到 3s,否则板上有负载时常驻内核会被默认(短)
     * 超时杀掉 → 握手中途各核停止应答(ready_cnt 随机)→ 507018。此前定义了宏却从未调用。 */
    {
        uint64_t _actual_to = 0;
        aclError _to_rc = aclrtSetOpExecuteTimeOutV2(PLATFORM_OP_EXECUTE_TIMEOUT_US, &_actual_to);
        if (_to_rc != ACL_SUCCESS) {
            fprintf(stderr, "[esl_proxy] aclrtSetOpExecuteTimeOutV2(%llu us) failed: %d\n",
                    (unsigned long long)PLATFORM_OP_EXECUTE_TIMEOUT_US, (int)_to_rc);
        } else {
            fprintf(stderr, "[esl_proxy] op-execute timeout set: requested=%llu us actual=%llu us\n",
                    (unsigned long long)PLATFORM_OP_EXECUTE_TIMEOUT_US, (unsigned long long)_actual_to);
        }
    }
    ACL_CHECK(aclrtCreateStream(&stream_aicpu), "aclrtCreateStream aicpu");
    ACL_CHECK(aclrtCreateStream(&stream_aicore), "aclrtCreateStream aicore");

    loader = esl_aicpu_loader_create();
    rc = esl_aicpu_loader_bootstrap(loader, dispatcher_bytes, dispatcher_len,
        aicpu_bytes, aicpu_len, stream_aicpu, device_id);
    if (rc != 0) {
        fprintf(stderr, "bootstrap failed: %d\n", rc);
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }
    rc = esl_aicpu_loader_init(loader);
    if (rc != 0) {
        fprintf(stderr, "loader init failed: %d\n", rc);
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    rc = esl_host_init_aicore_regs((uint64_t)device_id, &dev_regs);
    if (rc != 0) {
        fprintf(stderr, "esl_host_init_aicore_regs failed: %d\n", rc);
        esl_aicpu_loader_destroy(loader);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    memset(&host_runtime, 0, sizeof(host_runtime));
    host_runtime.worker_count = ESL_PROXY_ONBOARD_WORKER_COUNT;
    host_runtime.aicpu_thread_num = ESL_PROXY_AICPU_THREAD_NUM;
    /* 探测 AICPU OCCUPY 掩码,填入允许的控制核与 launch_count(失败则回退老行为)。 */
    esl_host_probe_aicpu_allowed(device_id, &host_runtime, ESL_PROXY_AICPU_THREAD_NUM);

    ACL_CHECK(devmem_alloc(&dev_runtime, sizeof(EslRuntime)), "runtime GM");
    ACL_CHECK(devmem_alloc(&dev_wall, ESL_DEVICE_WALL_SLOTS * sizeof(uint64_t)),
        "device stats GM");
    ACL_CHECK(devmem_alloc(&dev_k_args, sizeof(EslKernelArgs)),
        "device KernelArgs GM");
    payload_bytes = (size_t)ESL_PROXY_ONBOARD_WORKER_COUNT * 2U * sizeof(EslDispatchPayload);
    ACL_CHECK(devmem_alloc(&dev_payload, payload_bytes), "fake task args GM");
    ACL_CHECK(aclrtMemset(dev_payload.ptr, payload_bytes, 0, payload_bytes),
        "zero payload GM");

    for (i = 0; i < ESL_PROXY_ONBOARD_WORKER_COUNT; ++i) {
        uint8_t *base = (uint8_t *)dev_payload.ptr +
                        (size_t)i * 2U * sizeof(EslDispatchPayload);

        host_runtime.workers[i].task = (uint64_t)(uintptr_t)base;
        host_runtime.workers[i].core_type =
            (i < ESL_PROXY_ONBOARD_BLOCK_DIM) ? 0 : 1;
        /* sentinel: AICore overwrites with real physical_core_id at handshake phase 2 */
        host_runtime.workers[i].physical_core_id = 0xFFFFFFFFU;
    }

    ACL_CHECK(aclrtMemcpy(dev_runtime.ptr, sizeof(EslRuntime), &host_runtime,
                  sizeof(EslRuntime), ACL_MEMCPY_HOST_TO_DEVICE),
        "H2D runtime");
    ACL_CHECK(
        aclrtMemset(dev_wall.ptr, ESL_DEVICE_WALL_SLOTS * sizeof(uint64_t), 0,
            ESL_DEVICE_WALL_SLOTS * sizeof(uint64_t)),
        "zero stats");

    rc = ESL_SWIMLANE_HOST_ONBOARD_BEGIN(device_id, ".");
    if (rc != 0) {
        fprintf(stderr, "[esl_proxy] swimlane init failed: %d\n", rc);
        esl_aicpu_loader_destroy(loader);
        devmem_free(&dev_payload);
        devmem_free(&dev_k_args);
        devmem_free(&dev_wall);
        devmem_free(&dev_runtime);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    memset(&k_args, 0, sizeof(k_args));
    k_args.runtime_args = (struct EslRuntime *)dev_runtime.ptr;
    k_args.regs = dev_regs;
    k_args.log_level = ESL_PROXY_KARGS_LOG_LEVEL;
    k_args.log_info_v = 5;
    k_args.device_wall_data_base = (uint64_t)(uintptr_t)dev_wall.ptr;
    k_args.device_id = (uint32_t)device_id;
    /* 关键(对齐 simpler):FFTS+ C2C 控制地址。AICore mix(AIC+AIV)各 block 靠它做跨块
     * 屏障同步;此前从未填,AICore 收到 ffts_base_addr=0 → set_ffts_base_addr(0) → 块间同步失效
     * → block 0 与 block 1..23 分裂成两组、只一组起得来(ready_cnt 3/69 交替)→ 握手超时 507018。 */
    {
        uint64_t _ffts_base = 0;
        uint32_t _ffts_len = 0;
        rtError_t _ffts_rc = rtGetC2cCtrlAddr(&_ffts_base, &_ffts_len);
        if (_ffts_rc != RT_ERROR_NONE) {
            fprintf(stderr, "[esl_proxy] rtGetC2cCtrlAddr failed: %d (FFTS 块间同步将失效)\n",
                    (int)_ffts_rc);
        }
        k_args.ffts_base_addr = _ffts_base;
        fprintf(stderr, "[esl_proxy] ffts_base_addr=0x%llx len=%u\n",
                (unsigned long long)_ffts_base, (unsigned)_ffts_len);
    }
    ESL_SWIMLANE_HOST_ONBOARD_FILL_KARGS(&k_args);
    ACL_CHECK(aclrtMemcpy(dev_k_args.ptr, sizeof(EslKernelArgs), &k_args,
                  sizeof(EslKernelArgs), ACL_MEMCPY_HOST_TO_DEVICE),
        "H2D KernelArgs");

    aicore = esl_aicore_launcher_create();
    rc = esl_aicore_launcher_load(aicore, aicore_bytes, aicore_len);
    if (rc != 0) {
        fprintf(stderr, "aicore load failed: %d\n", rc);
        esl_aicore_launcher_destroy(aicore);
        esl_aicpu_loader_destroy(loader);
        devmem_free(&dev_payload);
        devmem_free(&dev_k_args);
        devmem_free(&dev_wall);
        devmem_free(&dev_runtime);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    fprintf(stderr,
        "[esl_proxy] launching AICore kernel block_dim=%d (workers=%d)\n",
        ESL_PROXY_ONBOARD_BLOCK_DIM, ESL_PROXY_ONBOARD_WORKER_COUNT);
    fprintf(stderr, "[esl_proxy] aicpu init launch\n");
    ESL_SWIMLANE_HOST_START();
    rc = esl_aicpu_loader_launch(loader, stream_aicpu, &k_args, 1,
        ESL_AICPU_INIT_NAME);
    if (rc != 0) {
        esl_aicore_launcher_destroy(aicore);
        esl_aicpu_loader_destroy(loader);
        devmem_free(&dev_payload);
        devmem_free(&dev_k_args);
        devmem_free(&dev_wall);
        devmem_free(&dev_runtime);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }
    fprintf(stderr, "[esl_proxy] aicpu init sync\n");
    ESL_HOST_SYNC_STREAM(stream_aicpu, "sync after aicpu init");

    ESL_SWIMLANE_HOST_ONBOARD_SYNC_CORE_TYPES(dev_runtime.ptr);

    /* Launch AICore BEFORE the AICPU exec kernel: the AICore cores first spin on
     * `aicpu_ready` (set by the exec handshake), so making them resident first
     * removes the launch-latency window where the exec handshake busy-waits for
     * not-yet-scheduled cores and trips the AICPU watchdog (rt err 507018). */
    fprintf(stderr,
        "[esl_proxy] aicore launch block_dim=%d (stream_aicore, before exec so "
        "cores are resident for handshake)\n",
        ESL_PROXY_ONBOARD_BLOCK_DIM);
    rc = esl_aicore_launcher_launch(aicore, stream_aicore, dev_k_args.ptr,
        ESL_PROXY_ONBOARD_BLOCK_DIM);
    if (rc != 0) {
        esl_aicore_launcher_destroy(aicore);
        esl_aicpu_loader_destroy(loader);
        devmem_free(&dev_payload);
        devmem_free(&dev_k_args);
        devmem_free(&dev_wall);
        devmem_free(&dev_runtime);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    {
        host_runtime.func_id_to_addr_[0] = 1U;
        host_runtime.func_id_to_addr_[1] = 1U;
        ACL_CHECK(aclrtMemcpy(dev_runtime.ptr, sizeof(EslRuntime), &host_runtime, sizeof(EslRuntime),
                      ACL_MEMCPY_HOST_TO_DEVICE),
            "H2D runtime (fake kernel enabled)");
    }

    fprintf(stderr, "[esl_proxy] aicpu exec launch (after aicore; aicore waits "
                    "on aicpu_ready)\n");
    rc = esl_aicpu_loader_launch(loader, stream_aicpu, &k_args,
        host_runtime.aicpu_launch_count, ESL_AICPU_EXEC_NAME);
    if (rc != 0) {
        esl_aicore_launcher_destroy(aicore);
        esl_aicpu_loader_destroy(loader);
        devmem_free(&dev_payload);
        devmem_free(&dev_k_args);
        devmem_free(&dev_wall);
        devmem_free(&dev_runtime);
        aclrtDestroyStream(stream_aicore);
        aclrtDestroyStream(stream_aicpu);
        aclrtResetDevice(device_id);
        aclFinalize();
        free(dispatcher_bytes);
        free(aicpu_bytes);
        free(aicore_bytes);
        return 1;
    }

    fprintf(stderr, "[esl_proxy] aicpu exec sync\n");
    ESL_HOST_SYNC_STREAM_DUMP(stream_aicpu, "sync after aicpu exec",
        dev_wall.ptr);
    /* 对齐 simpler sync_run_streams:AICPU 退出时已让所有 AICore 核 EXIT,这里再 sync
     * AICore 流,确保 kernel 完全跑完、GM 写完后再读 swimlane/stats、再 reset(否则
     * AICore 残留状态会带进下一个进程)。带超时,卡核时按失败处理。 */
    ESL_HOST_SYNC_STREAM(stream_aicore, "sync after aicore exit");
    ESL_SWIMLANE_HOST_ONBOARD_END();
    fprintf(stderr, "[esl_proxy] done (aicore stream synced)\n");
    fflush(stderr);

    memset(stats, 0, sizeof(stats));
    stats_rc = aclrtMemcpy(stats, sizeof(stats), dev_wall.ptr, sizeof(stats),
        ACL_MEMCPY_DEVICE_TO_HOST);
    if (stats_rc != ACL_SUCCESS) {
        fprintf(stderr, "D2H stats failed: %d\n", (int)stats_rc);
    }

    printf("esl_proxy onboard: task_cnt=%llu subtask_cnt=%llu completed_cnt=%llu "
           "wall_ns=%llu\n",
        (unsigned long long)stats[0], (unsigned long long)stats[1],
        (unsigned long long)stats[2], (unsigned long long)stats[3]);
    /* stats[4] = commit (low32). */
    unsigned long long commit_lo = (unsigned long long)(stats[4] & 0xffffffffULL);
    printf("[diag] commit=%llu n_uncomp=%llu first_uncomp=%u pred_cnt[first]=%u "
           "ready_cube=%u ready_vec=%u\n",
        commit_lo, (unsigned long long)stats[5],
        (unsigned)(stats[6] & 0xffffffffULL), (unsigned)(stats[6] >> 32),
        (unsigned)(stats[7] & 0xffffffffULL), (unsigned)(stats[7] >> 32));
    fflush(stdout);

    free(dispatcher_bytes);
    free(aicpu_bytes);
    free(aicore_bytes);

    /* The AICore kernel loops until device reset (no exit signal is sent), so we
     * MUST reset the device before exiting — otherwise the lingering AICore
     * kernel stays resident on the cores and the NEXT run's AICore launch /
     * handshake collides with it (intermittent rt err 507018 on repeated runs
     * sharing one device lock). */
    if (stats[0] == 0 || stats[2] != stats[0]) {
        fprintf(stderr,
            "esl_proxy onboard: FAIL (completed %llu != task_cnt %llu — DAG "
            "not fully scheduled)\n",
            (unsigned long long)stats[2], (unsigned long long)stats[0]);
        fflush(stderr);
        aclrtResetDevice(device_id);
        aclFinalize();
        _Exit(1);
    }
    printf("esl_proxy onboard: OK (all %llu tasks / %llu subtasks completed)\n",
        (unsigned long long)stats[0], (unsigned long long)stats[1]);
    fflush(stdout);
    aclrtResetDevice(device_id);
    aclFinalize();
    _Exit(0);
}
