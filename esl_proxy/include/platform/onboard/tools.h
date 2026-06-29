/*
 * Shared onboard utilities: file I/O, fingerprinting, dispatcher offsets.
 * Implementations live in src/onboard/tools.c.
 * AICPU CANN logging lives in onboard_log.h (implementation in onboard_log.c).
 */
#ifndef ESL_PROXY_ONBOARD_TOOLS_H
#define ESL_PROXY_ONBOARD_TOOLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "onboard_config.h"

static inline double cycles_to_us(uint64_t cycles)
{
    return ((double)cycles / (double)PLATFORM_PROF_SYS_CNT_FREQ) * 1000000.0;
}

static inline uint64_t esl_onboard_sys_cnt(void)
{
    uint64_t ticks;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
}

static inline uint64_t esl_onboard_time_ns(void)
{
    return esl_onboard_sys_cnt() * 1000000000ULL / PLATFORM_PROF_SYS_CNT_FREQ;
}

#ifdef __linux__
#include <sys/syscall.h>
#define GET_TID() syscall(SYS_gettid)
#else
#define GET_TID() 0
#endif

#define DISPATCHER_KERNEL_ARGS_DEVICE_ARGS_OFF 40
#define DISPATCHER_DEVICE_ARGS_AICPU_SO_BIN_OFF 96
#define DISPATCHER_DEVICE_ARGS_AICPU_SO_LEN_OFF 104
#define DISPATCHER_DEVICE_ARGS_DEVICE_ID_OFF 112
#define DISPATCHER_DEVICE_ARGS_INNER_SO_BIN_OFF 120
#define DISPATCHER_DEVICE_ARGS_INNER_SO_LEN_OFF 128

#define ESL_AICPU_INIT_NAME "esl_aicpu_init"
#define ESL_AICPU_EXEC_NAME "esl_aicpu_exec"
#define ESL_INNER_SO_BASENAME_FMT "esl_inner_%016lx_%d.so"
#define ESL_AICPU_DISPATCHER_SO_BASENAME "libesl_aicpu_dispatcher.so"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t fnv1a64(const char *data, size_t len);
uint64_t esl_fingerprint_bytes(const void *data, size_t len);
int64_t *grow_array(int64_t **arr, size_t *cap, size_t *len, int64_t value);
int read_file(const char *path, char **out_data, size_t *out_len);
int esl_elf_lookup_symbol(const char *data, size_t len, const char *name, uint64_t *out_value);
int write_bytes(const char *path, const char *data, uint64_t len);
void esl_make_inner_basename(uint64_t fp, int device_id, char *buf, size_t buf_size);
void esl_make_aicpu_op_type(const char *base, uint64_t fp, char *buf, size_t buf_size);
int esl_write_aicpu_kernel_json(const char *path, const char *kernel_so, uint64_t fp);

#ifdef ESL_PROXY_ONBOARD_HOST
void esl_host_dump_device_wall(const void *dev_wall_ptr);
#endif

#ifdef __cplusplus
}
#endif

static inline uint32_t reg_offset(RegId reg)
{
    switch (reg) {
    case REG_ID_DATA_MAIN_BASE:
        return REG_SPR_DATA_MAIN_BASE_OFFSET;
    case REG_ID_COND:
        return REG_SPR_COND_OFFSET;
    case REG_ID_FAST_PATH_ENABLE:
        return REG_SPR_FAST_PATH_ENABLE_OFFSET;
    default:
        return 0U;
    }
}

#endif /* ESL_PROXY_ONBOARD_TOOLS_H */
