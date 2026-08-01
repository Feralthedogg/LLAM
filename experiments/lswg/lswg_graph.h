/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#ifndef LLAM_EXPERIMENTS_LSWG_GRAPH_H
#define LLAM_EXPERIMENTS_LSWG_GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSWG_MAX_REPORT_MEMBERS 16U

typedef enum lswg_status {
    LSWG_STATUS_OK = 0,
    LSWG_STATUS_INVALID_ARGUMENT,
    LSWG_STATUS_OUT_OF_MEMORY,
    LSWG_STATUS_CAPACITY,
    LSWG_STATUS_DUPLICATE_IDENTITY,
    LSWG_STATUS_MISSING_ENDPOINT,
    LSWG_STATUS_INVALID_KIND,
    LSWG_STATUS_MALFORMED_SELECT,
    LSWG_STATUS_NOT_FINALIZED
} lswg_status_t;

typedef enum lswg_node_kind {
    LSWG_NODE_TASK = 0,
    LSWG_NODE_MUTEX,
    LSWG_NODE_COND,
    LSWG_NODE_CHANNEL,
    LSWG_NODE_SELECT,
    LSWG_NODE_IO_REQ,
    LSWG_NODE_TIMER,
    LSWG_NODE_BLOCK_JOB,
    LSWG_NODE_CANCEL_TOKEN,
    LSWG_NODE_EXTERNAL_SOURCE,
    LSWG_NODE_BACKEND_SOURCE,
    LSWG_NODE_KIND_COUNT
} lswg_node_kind_t;

typedef enum lswg_edge_kind {
    LSWG_EDGE_TASK_WAITS_MUTEX = 0,
    LSWG_EDGE_MUTEX_OWNED_BY_TASK,
    LSWG_EDGE_TASK_WAITS_JOIN,
    LSWG_EDGE_TASK_WAITS_CHANNEL_SEND,
    LSWG_EDGE_TASK_WAITS_CHANNEL_RECV,
    LSWG_EDGE_TASK_WAITS_SELECT,
    LSWG_EDGE_SELECT_ALTERNATIVE,
    LSWG_EDGE_CHANNEL_MATCHED_BY_TASK,
    LSWG_EDGE_TASK_WAITS_COND,
    LSWG_EDGE_TASK_WAITS_IO,
    LSWG_EDGE_IO_OWNED_BY_NODE,
    LSWG_EDGE_TASK_WAITS_TIMER,
    LSWG_EDGE_TASK_WAITS_BLOCK_JOB,
    LSWG_EDGE_TASK_CAN_CANCEL,
    LSWG_EDGE_RESOURCE_EXTERNAL_SIGNAL,
    LSWG_EDGE_KIND_COUNT
} lswg_edge_kind_t;

enum {
    LSWG_EDGE_AND_REQUIRED = 1U << 0,
    LSWG_EDGE_OR_ALTERNATIVE = 1U << 1,
    LSWG_EDGE_EXTERNAL_OPEN = 1U << 2,
    LSWG_EDGE_READY_NOW = 1U << 3,
    LSWG_EDGE_INCONSISTENT = 1U << 4,
    LSWG_EDGE_GENERATION_STABLE = 1U << 5
};

enum {
    LSWG_NODE_RUNNABLE = 1U << 0,
    LSWG_NODE_FUTURE_TIMER = 1U << 1,
    LSWG_NODE_LIVE_BACKEND = 1U << 2,
    LSWG_NODE_RUNNING_JOB = 1U << 3,
    LSWG_NODE_UNCANCELLED_TOKEN = 1U << 4,
    LSWG_NODE_READY_NOW = 1U << 5,
    LSWG_NODE_EXTERNAL_OPEN = 1U << 6,
    LSWG_NODE_TERMINAL_PROGRESS = 1U << 7,
    LSWG_NODE_MATCHABLE_STUCK = 1U << 8,
    LSWG_NODE_OVERDUE_STUCK = 1U << 9
};

enum {
    LSWG_INCOMPLETE_NONE = 0U,
    LSWG_INCOMPLETE_LOCK_BUSY = 1U << 0,
    LSWG_INCOMPLETE_ALLOCATION = 1U << 1,
    LSWG_INCOMPLETE_WORKSPACE = 1U << 2,
    LSWG_INCOMPLETE_UNSTABLE_GENERATION = 1U << 3,
    LSWG_INCOMPLETE_SHUTDOWN = 1U << 4,
    LSWG_INCOMPLETE_MALFORMED = 1U << 5
};

typedef enum lswg_verdict {
    LSWG_VERDICT_NONE = 0,
    LSWG_VERDICT_OPEN,
    LSWG_VERDICT_PROGRESS_CHANGED,
    LSWG_VERDICT_INCOMPLETE,
    LSWG_VERDICT_PROVEN_CYCLE,
    LSWG_VERDICT_PROVEN_ORPHAN,
    LSWG_VERDICT_MATCHABLE_LOST_WAKE,
    LSWG_VERDICT_OVERDUE_SOURCE
} lswg_verdict_t;

typedef struct lswg_identity {
    uint64_t primary;
    uint64_t generation;
    uint64_t auxiliary;
} lswg_identity_t;

typedef struct lswg_node_ref {
    lswg_node_kind_t kind;
    lswg_identity_t identity;
} lswg_node_ref_t;

typedef struct lswg_node_desc {
    lswg_node_ref_t ref;
    uint32_t semantic_flags;
    uint64_t semantic_value;
    uintptr_t raw_address;
} lswg_node_desc_t;

typedef struct lswg_edge_desc {
    lswg_node_ref_t source;
    lswg_node_ref_t target;
    lswg_edge_kind_t kind;
    uint32_t flags;
} lswg_edge_desc_t;

typedef void *(*lswg_allocate_fn)(void *context, size_t size);
typedef void (*lswg_deallocate_fn)(void *context, void *pointer);

typedef struct lswg_allocator {
    void *context;
    lswg_allocate_fn allocate;
    lswg_deallocate_fn deallocate;
} lswg_allocator_t;

typedef struct lswg_node {
    lswg_node_desc_t desc;
} lswg_node_t;

typedef struct lswg_edge {
    lswg_edge_desc_t desc;
    size_t source_index;
    size_t target_index;
} lswg_edge_t;

typedef struct lswg_graph {
    lswg_node_t *nodes;
    lswg_edge_t *edges;
    size_t node_count;
    size_t edge_count;
    size_t node_capacity;
    size_t edge_capacity;
    uint32_t incomplete_reasons;
    uint64_t capture_seq;
    bool finalized;
    lswg_allocator_t allocator;
} lswg_graph_t;

typedef struct lswg_workspace {
    uint8_t *escapable;
    uint8_t *on_stack;
    uint32_t *tarjan_index;
    uint32_t *tarjan_lowlink;
    size_t *tarjan_stack;
    size_t *queue;
    size_t *reverse_head;
    size_t *reverse_next;
    size_t *out_begin;
    size_t *dfs_nodes;
    size_t *dfs_edges;
    size_t node_capacity;
    size_t edge_capacity;
    lswg_allocator_t allocator;
} lswg_workspace_t;

typedef struct lswg_result {
    lswg_verdict_t verdict;
    uint32_t incomplete_reasons;
    uint64_t fingerprint;
    size_t member_count;
    size_t total_member_count;
    bool members_truncated;
    bool confirmed;
    lswg_node_ref_t members[LSWG_MAX_REPORT_MEMBERS];
} lswg_result_t;

typedef enum lswg_synthetic_profile {
    LSWG_SYNTHETIC_LONG_CHAIN_OPEN = 0,
    LSWG_SYNTHETIC_MANY_SCCS,
    LSWG_SYNTHETIC_OR_FANOUT_OPEN,
    LSWG_SYNTHETIC_MOSTLY_OPEN_WITH_ORPHAN
} lswg_synthetic_profile_t;

lswg_status_t lswg_graph_init(lswg_graph_t *graph,
                              size_t node_capacity,
                              size_t edge_capacity,
                              const lswg_allocator_t *allocator);
void lswg_graph_destroy(lswg_graph_t *graph);
void lswg_graph_reset(lswg_graph_t *graph);
lswg_status_t lswg_graph_add_node(lswg_graph_t *graph,
                                  const lswg_node_desc_t *node);
lswg_status_t lswg_graph_add_edge(lswg_graph_t *graph,
                                  const lswg_edge_desc_t *edge);
void lswg_graph_mark_incomplete(lswg_graph_t *graph, uint32_t reasons);
lswg_status_t lswg_graph_finalize(lswg_graph_t *graph);

lswg_status_t lswg_workspace_init(lswg_workspace_t *workspace,
                                  size_t node_capacity,
                                  size_t edge_capacity,
                                  const lswg_allocator_t *allocator);
void lswg_workspace_destroy(lswg_workspace_t *workspace);

lswg_status_t lswg_fingerprint(const lswg_graph_t *graph,
                               uint64_t *fingerprint_out);
lswg_status_t lswg_solve(const lswg_graph_t *graph,
                         lswg_workspace_t *workspace,
                         lswg_result_t *result);
lswg_status_t lswg_confirm(const lswg_result_t *first,
                           uint64_t first_coarse_progress,
                           const lswg_result_t *second,
                           uint64_t second_coarse_progress,
                           lswg_result_t *confirmed);

lswg_status_t lswg_synthetic_populate(lswg_graph_t *graph,
                                      size_t node_count,
                                      lswg_synthetic_profile_t profile);

#ifdef __cplusplus
}
#endif

#endif
