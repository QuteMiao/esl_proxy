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
    atomic_thread_fence(memory_order_seq_cst);
}

#endif /* SPIN_H */
