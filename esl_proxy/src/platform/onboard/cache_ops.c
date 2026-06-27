#include "platform.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Single GM cache-maintenance primitive: clean+invalidate (dc civac) by cache line.
 *
 * Callers MUST pass a 64B-aligned base and a non-zero, 64B-multiple size — every
 * object handed to this primitive is a whole, cache-line-aligned region (see the
 * __attribute__((aligned(64))) per-task structs in task.h / runtime.h). No runtime
 * rounding is performed.
 *
 * `dc civac` is a superset of plain clean (`dc cvac`): it works for both publish
 * (AICPU writes, AICore reads) and consume (invalidate before reading AICore writes),
 * matching the AICore side which publishes with dcci(..., CACHELINE_OUT). `dsb sy`
 * orders the maintenance to PoC against the non-coherent AICore; no `isb` is needed
 * (it synchronises the instruction stream / context, not DMA-visible data).
 */
/* Issue dc civac over [addr, addr+size) WITHOUT the dsb sy barrier.
 * For batching several regions under a single trailing cache_civac_barrier(). */
void cache_civac_lines(const void *addr, size_t size)
{
    uintptr_t p = (uintptr_t)addr;
    uintptr_t end = p + size;

    for (; p < end; p += 64) {
        __asm__ __volatile__("dc civac, %0" ::"r"(p) : "memory");
    }
}

/* One full-system barrier: completes all preceding dc civac to PoC. */
void cache_civac_barrier(void)
{
    __asm__ __volatile__("dsb sy" ::: "memory");
}

void cache_civac_range(const void *addr, size_t size)
{
    cache_civac_lines(addr, size);
    cache_civac_barrier();
}
