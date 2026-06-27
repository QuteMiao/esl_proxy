#include "aicore.h"

#include <stdint.h>

__thread SimCoreReg *g_sim_aicore_tls_reg;
__thread uint32_t g_sim_aicore_tls_phys;

void sim_aicore_tls_set(SimCoreReg *reg, uint32_t physical_core_id)
{
    g_sim_aicore_tls_reg = reg;
    g_sim_aicore_tls_phys = physical_core_id;
}
