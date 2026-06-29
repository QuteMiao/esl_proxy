#include "platform.h"

#include <stddef.h>
#include <stdint.h>

void cache_invalidate_range(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
    __asm__ __volatile__("" ::: "memory");
}

void cache_flush_range(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
    __asm__ __volatile__("" ::: "memory");
}

void cache_civac_lines(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
    __asm__ __volatile__("" ::: "memory");
}

void cache_civac_barrier(void)
{
    __asm__ __volatile__("" ::: "memory");
}
