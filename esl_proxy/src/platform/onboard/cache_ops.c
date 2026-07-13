#include "platform.h"

#include <stddef.h>
#include <stdint.h>

void cache_invalidate_range(const void *addr, size_t size)
{
    const size_t k_cache_line_size = 64;
    uintptr_t start;
    uintptr_t end;
    uintptr_t p;

    if (size == 0) {
        return;
    }
    start = (uintptr_t)addr & ~(k_cache_line_size - 1);
    end = ((uintptr_t)addr + size + k_cache_line_size - 1) & ~(k_cache_line_size - 1);
    for (p = start; p < end; p += k_cache_line_size) {
        __asm__ __volatile__("dc civac, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

void cache_flush_range(const void *addr, size_t size)
{
    const size_t k_cache_line_size = 64;
    uintptr_t start;
    uintptr_t end;
    uintptr_t p;

    if (size == 0) {
        return;
    }
    start = (uintptr_t)addr & ~(k_cache_line_size - 1);
    end = ((uintptr_t)addr + size + k_cache_line_size - 1) & ~(k_cache_line_size - 1);
    for (p = start; p < end; p += k_cache_line_size) {
        __asm__ __volatile__("dc cvac, %0" ::"r"(p) : "memory");
    }
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}

/* Issue dc civac over [addr, addr+size) WITHOUT the dsb sy/isb barrier, so several
 * regions can be batched under a single trailing cache_civac_barrier(). */
void cache_civac_lines(const void *addr, size_t size)
{
    const size_t k_cache_line_size = 64;
    uintptr_t start;
    uintptr_t end;
    uintptr_t p;

    if (size == 0) {
        return;
    }
    start = (uintptr_t)addr & ~(k_cache_line_size - 1);
    end = ((uintptr_t)addr + size + k_cache_line_size - 1) & ~(k_cache_line_size - 1);
    for (p = start; p < end; p += k_cache_line_size) {
        __asm__ __volatile__("dc civac, %0" ::"r"(p) : "memory");
    }
}

/* One barrier: completes all preceding dc civac to PoC. */
void cache_civac_barrier(void)
{
    __asm__ __volatile__("dsb sy" ::: "memory");
    __asm__ __volatile__("isb" ::: "memory");
}
