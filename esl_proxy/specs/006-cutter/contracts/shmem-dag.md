# Shared Memory Contract: DAG Graph

**Feature**: 006-cutter | **Contract**: Cutter ← Orchestrator (task graph)

## Overview

The Cutter reads the task dependency graph from a shared memory region written by the Orchestrator. The graph is a DAG with task nodes and directed edges representing precedence constraints.

## Memory Region

**Key**: `shmem://cutter-dag/<pipeline_id>` (configured at startup)

## Data Format

```c
// Layout (header-only, defined in include/shmem_layout.h)
typedef struct {
    uint32_t node_count;                // Total task count in graph
    uint32_t edge_count;                // Total edge count in graph
    struct task_desc nodes[];          // Task descriptors (node_count entries)
    successorList successors[];         // Per-task successor adjacency (node_count entries)
} dag_region_t;
```

### Fields

| Field | Type | Description |
|-------|------|-------------|
| `node_count` | `uint32_t` | Number of tasks (nodes) in the DAG |
| `edge_count` | `uint32_t` | Number of directed edges in the DAG |
| `nodes[i]` | `struct task_desc` | Task descriptor for task `i` (2-byte `id`, `type`, `mode`, `kernel`, etc.) |
| `successors[i]` | `successorList` | Adjacency list: direct successors of task `i` |

### Successor List Node

```c
struct successorList {
    uint16_t successor[3];       // Up to 3 direct successors
    struct successorList *next;  // Overflow chain (NULL if no more)
};
```

Each task can have up to 3 successors per node; overflow chains via `next` pointer for nodes with >3 successors.

## Read Protocol (Cutter)

1. Load `node_count` with `memory_order_acquire`
2. Load `edge_count` with `memory_order_acquire`
3. For each task `i` in `[0, node_count)`:
   a. Read `nodes[i].id` to confirm task ID
   b. Walk `successors[i]` linked list:
      - For each `successor[j]` (j=0..2, non-zero), record edge `i → successor[j]`
4. Build local dependency state: `remaining_pred[task_id] = in-degree of task`

## DAG Invariants

- **Acyclic**: Orchestrator guarantees no cycles at graph construction time
- **Topological**: Cutter processes edges in dependency order (task A completes before task B if B depends on A)
- **Bounded**: `node_count <= 10000`, `edge_count <= 50000`

## Graph Construction Contract (Orchestrator)

- Orchestrator writes `node_count` and `edge_count` first (barrier)
- Orchestrator writes all `nodes[]` descriptors
- Orchestrator writes all `successors[]` adjacency lists
- All writes use `memory_order_release` before region is considered "published"
- Graph updates (adding tasks): Orchestrator appends to end, updates `node_count` atomically

## Synchronization

- **Read-only for Cutter**: Cutter does not modify DAG region
- **Lock-free read**: `uint32_t` loads are naturally aligned and atomic
- **Memory ordering**: `memory_order_acquire` on all reads to ensure visibility of Orchestrator's writes

## Constraints

- **Trust the Caller**: Cutter assumes Orchestrator provides a valid DAG (no cycles, all successor IDs in range)
- **No edge validation**: Cutter does not verify acyclicity (assumed pre-checked by Orchestrator)
- **Read-only**: Cutter never writes to this region
