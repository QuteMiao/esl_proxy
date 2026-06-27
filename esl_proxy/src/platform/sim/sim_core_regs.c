#include "sim_core_regs.h"

#include "platform_config.h"
#include "platform_regs.h"

#include <stddef.h>
#include <stdint.h>

static SimCoreReg g_sim_core_regs[ESL_PROXY_PLATFORM_HAL_REG_SLOTS];
static uint64_t g_sim_reg_table[ESL_PROXY_PLATFORM_HAL_REG_SLOTS];

void sim_core_regs_init(void)
{
    int s;

    for (s = 0; s < (int)ESL_PROXY_PLATFORM_HAL_REG_SLOTS; ++s) {
        g_sim_core_regs[s].data_main_base = 0;
        g_sim_core_regs[s].cond = 0;
        g_sim_core_regs[s].exited = 0;
        g_sim_reg_table[s] = (uint64_t)(uintptr_t)&g_sim_core_regs[s];
    }
    set_platform_regs((uint64_t)(uintptr_t)g_sim_reg_table);
}

SimCoreReg *sim_core_reg_at(uint64_t reg_base_addr)
{
    if (reg_base_addr == 0) {
        return NULL;
    }
    return (SimCoreReg *)(uintptr_t)reg_base_addr;
}
