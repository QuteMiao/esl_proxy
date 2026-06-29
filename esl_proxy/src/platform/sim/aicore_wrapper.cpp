/*
 * aicore_wrapper.cpp — sim AICore worker thread entry (TLS reg + algorithm executor).
 */
#include "aicore_executor.h"

#include "platform_config.h"
#include "runtime.h"
#include "aicore.h"
#include "sim_core_regs.h"
#include "swimlane_aicore.h"

#include <cstdint>

static uint32_t s_aicore_profiling_flag;
static __gm__ uint64_t *s_l2_swimlane_aicore_head_slot;
static __gm__ L2SwimlaneActiveHead *s_l2_swimlane_aicore_head;

extern "C" __attribute__((weak)) void set_aicore_profiling_flag(uint32_t flag)
{
    s_aicore_profiling_flag = flag;
}

extern "C" __attribute__((weak)) uint32_t get_aicore_profiling_flag(void)
{
    return s_aicore_profiling_flag;
}

extern "C" __attribute__((weak)) void set_l2_swimlane_aicore_head_slot(__gm__ uint64_t *slot_ptr)
{
    s_l2_swimlane_aicore_head_slot = slot_ptr;
    s_l2_swimlane_aicore_head = nullptr;
}

extern "C" __attribute__((weak)) __gm__ L2SwimlaneActiveHead *get_l2_swimlane_aicore_head(void)
{
    if (s_l2_swimlane_aicore_head_slot == nullptr) {
        return nullptr;
    }
    if (s_l2_swimlane_aicore_head == nullptr) {
        s_l2_swimlane_aicore_head =
            reinterpret_cast<__gm__ L2SwimlaneActiveHead *>(*s_l2_swimlane_aicore_head_slot);
    }
    return s_l2_swimlane_aicore_head;
}

extern "C" void aicore_execute_wrapper(EslRuntime *runtime, int block_idx, CoreType core_type,
                                       uint32_t physical_core_id, uint64_t regs_table,
                                       uint32_t profiling_flag, uint64_t rotation_table)
{
    int hal_idx;
    uint64_t *table;
    SimCoreReg *reg;

    (void)physical_core_id;
    hal_idx = esl_worker_to_hal_reg_index(block_idx);
    if (hal_idx < 0 || regs_table == 0) {
        return;
    }
    table = reinterpret_cast<uint64_t *>(regs_table);
    reg = sim_core_reg_at(table[hal_idx]);
    sim_aicore_tls_set(reg, (uint32_t)hal_idx);

    set_aicore_profiling_flag(profiling_flag);
    if (ESL_SWIMLANE_IS_FLAG_ON(profiling_flag) && rotation_table != 0U) {
        uint64_t *head_table = reinterpret_cast<uint64_t *>(rotation_table);
        set_l2_swimlane_aicore_head_slot(reinterpret_cast<__gm__ uint64_t *>(&head_table[block_idx]));
    } else {
        set_l2_swimlane_aicore_head_slot(nullptr);
    }

    aicore_execute(runtime, block_idx, core_type, profiling_flag, rotation_table);
}
