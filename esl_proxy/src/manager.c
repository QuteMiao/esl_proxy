/*
 * mem_pool.c - Manager thread for automatic memory release
 *
 * Dedicated manager thread that monitors Task State Ring Buffer
 * and processes when2free FIFO entries to release memory.
 */

#include "manager.h"
#include "log.h"

/*
 * Manager thread entry point
 * Polls ring buffer for minimum uncompleted TaskID and processes when2free entries.
 */
void *manager_worker(void *arg)
{
    mem_pool_t *pool = (mem_pool_t *)arg;

    WORKER_LOGF("manager", "started pool=%p", (void *)pool);

    while (1) {
        mem_pool_process_when2free(pool);
    }

    return NULL;
}
