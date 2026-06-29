/* npu_hal.c — onboard MMIO register table. */
#define _GNU_SOURCE

#include "platform_regs.h"
#include "memory_barrier.h"
#include "tools.h" /* reg_offset() for get_reg_ptr */

#include <stddef.h>
#include <stdint.h>

static uint64_t g_platform_regs;

void set_platform_regs(uint64_t regs)
{
    g_platform_regs = regs;
}

uint64_t get_platform_regs(void)
{
    return g_platform_regs;
}

static volatile uint32_t *get_reg_ptr(uint64_t reg_base_addr, RegId reg)
{
    return (volatile uint32_t *)(uintptr_t)(reg_base_addr + reg_offset(reg));
}

uint64_t read_reg(uint64_t reg_base_addr, RegId reg)
{
    return (uint64_t)*get_reg_ptr(reg_base_addr, reg);
}

void write_reg(uint64_t reg_base_addr, RegId reg, uint64_t value)
{
    *get_reg_ptr(reg_base_addr, reg) = (uint32_t)value;
}

void platform_init_aicore_regs(uint64_t reg_addr)
{
    write_reg(reg_addr, REG_ID_FAST_PATH_ENABLE, REG_SPR_FAST_PATH_OPEN);
    write_reg(reg_addr, REG_ID_DATA_MAIN_BASE, AICPU_IDLE_TASK_ID);
}

int platform_reg_task_finished(uint64_t reg_base_addr, uint32_t reg_task)
{
    const uint64_t cond = read_reg(reg_base_addr, REG_ID_COND);

    OUT_OF_ORDER_LOAD_BARRIER();
    return (EXTRACT_TASK_STATE(cond) == TASK_FIN_STATE &&
            EXTRACT_TASK_ID(cond) == (int)reg_task)
               ? 1
               : 0;
}

int platform_reg_task_acked(uint64_t reg_base_addr, uint32_t reg_task)
{
    const uint64_t cond = read_reg(reg_base_addr, REG_ID_COND);

    OUT_OF_ORDER_LOAD_BARRIER();
    /* ACK or FIN of reg_task both set COND.id == reg_task. */
    return (EXTRACT_TASK_ID(cond) == (int)reg_task) ? 1 : 0;
}

void platform_reg_task_ack(uint64_t reg_base_addr, uint32_t reg_task)
{
    (void)reg_base_addr;
    (void)reg_task;
}

int platform_reg_worker_exit_acked(uint64_t reg_base_addr)
{
    const uint64_t cond = read_reg(reg_base_addr, REG_ID_COND);

    OUT_OF_ORDER_LOAD_BARRIER();
    return cond == (uint64_t)AICORE_EXITED_VALUE;
}

uint32_t platform_reg_cond_raw(uint64_t reg_base_addr)
{
    const uint64_t cond = read_reg(reg_base_addr, REG_ID_COND);

    OUT_OF_ORDER_LOAD_BARRIER();
    return (uint32_t)cond;
}
