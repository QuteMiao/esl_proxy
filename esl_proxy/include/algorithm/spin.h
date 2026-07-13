/*
 * spin.h - Atomic spin utilities
 *
 * Low-level atomic operations for spin waiting and synchronization.
 * C11 standard with stdatomic.
 */

#ifndef SPIN_H
#define SPIN_H

#include <stdatomic.h>

/*
 * Memory barrier for spin-wait loops
 */
static inline void spin_wait(void)
{
    /* Spin re-read barrier: downgraded seq_cst -> acquire. The loop bodies only
     * need load/load + load/store ordering to re-fetch shared state each spin;
     * a global total order is not required. Cross-core (non-coherent AICPU/GM)
     * visibility is provided by explicit wmb()/rmb()/cache_civac on the writer
     * side, not by this fence, so acquire is sufficient here. */
    atomic_thread_fence(memory_order_acquire);
}

#endif /* SPIN_H */