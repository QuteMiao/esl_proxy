/*
 * Host-side stub orchestration for esl_proxy onboard smoke.
 * Real orchestration runs on AICPU thread 2 inside libaicpu_kernel.so.
 */
#include <stdint.h>

extern "C" void aicpu_orchestration_entry(const uint64_t orch_args)
{
    (void)orch_args;
}
