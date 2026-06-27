#include "platform.h"

#include <stddef.h>
#include <stdint.h>

/* Sim backend: no real cache hierarchy shared with a non-coherent agent — compiler
 * barrier only. Mirrors the onboard cache_civac_range signature. */
void cache_civac_range(const void *addr, size_t size)
{
    (void)addr;
    (void)size;
    __asm__ __volatile__("" ::: "memory");
}
