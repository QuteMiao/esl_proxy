/*
 * dep_dump.h - Post-orchestration static DAG export
 *
 * Snapshot producer->consumer edges from ring buffer successor lists.
 * Enable at compile time via DEP_DUMP=1; trigger at runtime via DEP_DUMP=1 env.
 */

#ifndef DEP_DUMP_H
#define DEP_DUMP_H

#include <stdio.h>
#include <stdint.h>

#include "conf.h"

#if DEP_DUMP

uint32_t dep_dump_count_edges(void);

void dep_dump_edges_csv(FILE *out);
void dep_dump_tasks_csv(FILE *out);
void dep_dump_dot(FILE *out);
void dep_dump_summary(FILE *out);

/* Reads DEP_DUMP / DEP_DUMP_FILE / DEP_DUMP_FORMAT env; no-op if disabled. */
void dep_dump_maybe(void);

#else

static inline uint32_t dep_dump_count_edges(void) { return 0; }
static inline void dep_dump_edges_csv(FILE *out) { (void)out; }
static inline void dep_dump_tasks_csv(FILE *out) { (void)out; }
static inline void dep_dump_dot(FILE *out) { (void)out; }
static inline void dep_dump_summary(FILE *out) { (void)out; }
static inline void dep_dump_maybe(void) {}

#endif /* DEP_DUMP */

#endif /* DEP_DUMP_H */
