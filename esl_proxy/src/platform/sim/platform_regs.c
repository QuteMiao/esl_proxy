/*
 * Host-side platform register stubs for CPU sim (shared memory per HAL slot).
 */
#include "platform_regs.h"

#include "log.h"
#include "memory_barrier.h"
#include "platform_config.h"
#include "sim_core_regs.h"

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

uint64_t read_reg(uint64_t reg_base_addr, RegId reg)
{
    SimCoreReg *cr;

    cr = sim_core_reg_at(reg_base_addr);
    if (cr == NULL) {
        return 0;
    }
    OUT_OF_ORDER_LOAD_BARRIER();
    if (reg == REG_ID_COND) {
        return (uint64_t)cr->cond;
    }
    if (reg == REG_ID_DATA_MAIN_BASE) {
        if (cr->exited) {
            return (uint64_t)AICORE_EXIT_SIGNAL;
        }
        return (uint64_t)cr->data_main_base;
    }
    return 0;
}

void write_reg(uint64_t reg_base_addr, RegId reg, uint64_t value)
{
    SimCoreReg *cr;

    cr = sim_core_reg_at(reg_base_addr);
    if (cr == NULL) {
        return;
    }
    if (reg == REG_ID_DATA_MAIN_BASE) {
        if (value == (uint64_t)AICORE_EXIT_SIGNAL) {
            cr->exited = 1;
            cr->data_main_base = (uint32_t)value;
            ESL_WMB();
            return;
        }
        cr->exited = 0;
        cr->data_main_base = (uint32_t)value;
        ESL_WMB();
        return;
    }
    if (reg == REG_ID_COND) {
        cr->cond = (uint32_t)value;
        ESL_WMB();
    }
}

int platform_reg_task_finished(uint64_t reg_base_addr, uint32_t reg_task)
{
    SimCoreReg *cr;

    cr = sim_core_reg_at(reg_base_addr);
    if (cr == NULL || cr->exited) {
        return 0;
    }
    return cr->cond == (uint32_t)MAKE_FIN_VALUE(reg_task);
}

int platform_reg_task_acked(uint64_t reg_base_addr, uint32_t reg_task)
{
    SimCoreReg *cr;

    cr = sim_core_reg_at(reg_base_addr);
    if (cr == NULL || cr->exited) {
        return 0;
    }
    /* ACK or FIN of reg_task both set COND.id == reg_task. */
    return (EXTRACT_TASK_ID((uint64_t)cr->cond) == (int)reg_task) ? 1 : 0;
}

void platform_reg_task_ack(uint64_t reg_base_addr, uint32_t reg_task)
{
    SimCoreReg *cr;

    cr = sim_core_reg_at(reg_base_addr);
    if (cr == NULL) {
        return;
    }
    if (cr->cond == (uint32_t)MAKE_FIN_VALUE(reg_task)) {
        cr->cond = 0;
    }
}

int platform_reg_worker_exit_acked(uint64_t reg_base_addr)
{
    SimCoreReg *cr;

    cr = sim_core_reg_at(reg_base_addr);
    if (cr == NULL) {
        return 0;
    }
    OUT_OF_ORDER_LOAD_BARRIER();
    return cr->cond == (uint32_t)AICORE_EXITED_VALUE;
}

uint32_t platform_reg_cond_raw(uint64_t reg_base_addr)
{
    SimCoreReg *cr;

    cr = sim_core_reg_at(reg_base_addr);
    if (cr == NULL || cr->exited) {
        return 0U;
    }
    return cr->cond;
}

void platform_init_aicore_regs(uint64_t reg_addr)
{
    SimCoreReg *cr;

    cr = sim_core_reg_at(reg_addr);
    if (cr == NULL) {
        return;
    }
    cr->data_main_base = 0;
    cr->cond = 0;
    cr->exited = 0;
}
