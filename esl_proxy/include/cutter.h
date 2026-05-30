/*
 * cutter.h - Cutter: Dependency Resolution for DAG Engine
 *
 * Reads completed task results from shared memory (Dispatch),
 * reads task dependency graph (Orchestrator), resolves dependencies
 * to identify newly ready tasks, and writes ready-task notifications
 * to shared memory (Dispatch).
 *
 * Header-only library. Include cutter_impl.h for implementations.
 *
 * Naming follows Constitution XI: concise names within cutter module context.
 */

#ifndef DAG_CUTTER_H
#define DAG_CUTTER_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


/*
 * Initialize a Cutter context
 * Returns: pointer to cutter_ctx_t on success, NULL on failure
 *
 * Caller must call cutter_shutdown() to release resources
 */
static inline cutter_ctx_t *cutter_init(const cutter_cfg_t *restrict cfg);

/*
 * Read completed task IDs from shared memory
 * Returns: number of task IDs written to buf
 *
 * Parameters:
 *   ctx  - cutter context
 *   buf  - output buffer for task IDs
 *   max  - maximum number of task IDs to read
 */
static inline uint32_t cutter_read_complete(cutter_ctx_t *restrict ctx,
                                             uint16_t *restrict buf, uint32_t max);

/*
 * Resolve dependencies for a completed task
 * Walks successor list, decrements predecessor counts,
 * and collects newly ready tasks into the ready batch
 *
 * Parameters:
 *   ctx      - cutter context
 *   task_id  - task that has just completed
 */
static inline void cutter_resolve(cutter_ctx_t *restrict ctx, uint16_t task_id);

/*
 * Write ready-task notifications to shared memory
 * Writes all tasks collected in the ready batch since last call
 * Increments seq_num to signal Dispatch of new ready tasks
 */
static inline void cutter_write_ready(cutter_ctx_t *restrict ctx);


#endif /* DAG_CUTTER_H */
