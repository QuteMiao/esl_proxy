/*
 * Copyright (c) PyPTO Contributors.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 * -----------------------------------------------------------------------------------------------------------
 */
/**
 * @file aicore.h
 * @brief AICore Platform Abstraction Layer
 *
 * Memory qualifiers, register helpers, CoreType, and onboard config for
 * real Ascend hardware (ccec) and host simulation builds.
 */

#ifndef SRC_COMMON_PLATFORM_INCLUDE_AICORE_AICORE_H_
#define SRC_COMMON_PLATFORM_INCLUDE_AICORE_AICORE_H_

#include <stdint.h>

#include "onboard_config.h"
#include "core_type.h"

#ifndef __gm__
#define __gm__
#endif

#ifndef __global__
#define __global__
#endif

#ifndef __in__
#define __in__
#endif

#ifndef __out__
#define __out__
#endif

#ifndef __aicore__
#define __aicore__ [aicore]
#endif

#define SPIN_WAIT_HINT() ((void)0)
#define OUT_OF_ORDER_STORE_BARRIER() ((void)0)
#define OUT_OF_ORDER_LOAD_BARRIER() ((void)0)
#define OUT_OF_ORDER_FULL_BARRIER() ((void)0)

__aicore__ static inline uint64_t read_reg(RegId reg)
{
    switch (reg) {
    case REG_ID_DATA_MAIN_BASE: {
        uint32_t val;
        __asm__ volatile("MOV %0, DATA_MAIN_BASE\n" : "=l"(val));
        return (uint64_t)val;
    }
    case REG_ID_COND:
    case REG_ID_FAST_PATH_ENABLE:
        return 0;
    }
    return 0;
}

__aicore__ static inline void write_reg(RegId reg, uint64_t value)
{
    switch (reg) {
    case REG_ID_COND:
        set_cond((uint32_t)value);
        break;
    case REG_ID_DATA_MAIN_BASE:
    case REG_ID_FAST_PATH_ENABLE:
        break;
    }
}

__aicore__ static inline uint32_t get_physical_core_id(void)
{
    return (uint32_t)get_coreid() & AICORE_COREID_MASK;
}

__aicore__ __attribute__((always_inline)) static inline uint64_t get_sys_cnt_aicore(void)
{
    return get_sys_cnt();
}

__aicore__ __attribute__((always_inline)) static inline uint64_t esl_aicore_now_ns(void)
{
    return get_sys_cnt_aicore() * 1000000000ULL / ESL_ONBOARD_SYS_CNT_FREQ;
}

#endif /* SRC_COMMON_PLATFORM_INCLUDE_AICORE_AICORE_H_ */
