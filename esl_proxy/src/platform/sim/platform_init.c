/*
 * platform_init.c — sim host platform init (runtime layout + algorithm init + handshake).
 */
#include "platform.h"

#include "conf.h"
#include "cutter.h"
#include "device_runner.h"
#include "dispatch.h"
#include "executor.h"
#include "handshake.h"
#include "platform_config.h"
#include "platform_regs.h"
#include "ring_buf.h"
#include "sim_core_regs.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

void init_predecessors(void);

static EslDispatchPayload g_sim_payload[PLATFORM_HOST_WORKER_COUNT * 2U];

static void sim_runtime_setup(EslRuntime *rt, EslDispatchPayload *payload, int worker_count)
{
    int i;

    if (rt == NULL || payload == NULL || worker_count <= 0 ||
        worker_count > RUNTIME_MAX_WORKER) {
        return;
    }

    memset(rt, 0, sizeof(*rt));
    rt->worker_count = worker_count;
    rt->aicpu_thread_num = CUTTER_THREAD_CNT + DISPATCH_THREAD_CNT + 1;

    for (i = 0; i < worker_count; ++i) {
        rt->workers[i].task =
            (uint64_t)(uintptr_t)(payload + (size_t)i * 2U);
        rt->workers[i].core_type = (i < AIC_CNT) ? 0 : 1;
    }
}

void platform_handshake_aicore_bootstrap(EslRuntime *rt)
{
    (void)rt;
    sim_core_regs_init();
}

int esl_platform_init(EslRuntime *runtime)
{
    if (runtime == NULL) {
        return -1;
    }

    sim_runtime_setup(runtime, g_sim_payload, PLATFORM_HOST_WORKER_COUNT);
    runtime->func_id_to_addr_[0] = 1U;
    runtime->func_id_to_addr_[1] = 1U;
    esl_init_global_context(runtime);
    ring_buf_init();
    init_state_buf();
    init_predecessors();
    init_ctrl_t();
    executor_init();

    platform_handshake_aicore_bootstrap(runtime);
    if (get_platform_regs() != 0) {
        if (esl_sim_aicore_workers_start(runtime) != 0) {
            return -1;
        }
        if (esl_handshake_all_cores(runtime) != 0) {
            esl_sim_aicore_workers_stop();
            return -1;
        }
    }
    g_runtime = runtime;
    return 0;
}

void esl_platform_shutdown(EslRuntime *runtime)
{
    if (runtime != NULL) {
        esl_shutdown_all_cores(runtime);
    }
    esl_sim_aicore_workers_stop();
}

void platform_stats_publish(uint64_t task_cnt, uint64_t subtask_cnt, uint64_t completed_cnt,
                            uint64_t commit, uint64_t ready_cube, uint64_t ready_vec,
                            uint64_t min_uncomplete, uint64_t elapsed_ns)
{
    (void)task_cnt;
    (void)subtask_cnt;
    (void)completed_cnt;
    (void)commit;
    (void)ready_cube;
    (void)ready_vec;
    (void)min_uncomplete;
    (void)elapsed_ns;
}
