/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lswg_graph.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    LSWG_WORK_ESCAPABLE = 1U << 0,
    LSWG_WORK_RELEVANT = 1U << 1
};

static void *
workspace_default_allocate(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}

static void
workspace_default_deallocate(void *context, void *pointer)
{
    (void)context;
    free(pointer);
}

static lswg_status_t
workspace_select_allocator(const lswg_allocator_t *requested,
                           lswg_allocator_t *selected)
{
    if (selected == NULL) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    if (requested == NULL) {
        selected->context = NULL;
        selected->allocate = workspace_default_allocate;
        selected->deallocate = workspace_default_deallocate;
        return LSWG_STATUS_OK;
    }
    if (requested->allocate == NULL || requested->deallocate == NULL) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    *selected = *requested;
    return LSWG_STATUS_OK;
}

static bool
workspace_allocate_array(lswg_workspace_t *workspace, void **target,
                         size_t count, size_t element_size)
{
    size_t bytes;

    *target = NULL;
    if (count == 0U) {
        return true;
    }
    if (element_size != 0U && count > SIZE_MAX / element_size) {
        return false;
    }
    bytes = count * element_size;
    *target = workspace->allocator.allocate(workspace->allocator.context,
                                             bytes);
    return *target != NULL;
}

lswg_status_t
lswg_workspace_init(lswg_workspace_t *workspace, size_t node_capacity,
                    size_t edge_capacity, const lswg_allocator_t *allocator)
{
    lswg_allocator_t selected;

    if (workspace == NULL || node_capacity == SIZE_MAX ||
        node_capacity > UINT32_MAX ||
        workspace_select_allocator(allocator, &selected) != LSWG_STATUS_OK) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    memset(workspace, 0, sizeof(*workspace));
    workspace->allocator = selected;
    workspace->node_capacity = node_capacity;
    workspace->edge_capacity = edge_capacity;

    if (!workspace_allocate_array(workspace, (void **)&workspace->escapable,
                                  node_capacity, sizeof(*workspace->escapable)) ||
        !workspace_allocate_array(workspace, (void **)&workspace->on_stack,
                                  node_capacity, sizeof(*workspace->on_stack)) ||
        !workspace_allocate_array(workspace,
                                  (void **)&workspace->tarjan_index,
                                  node_capacity,
                                  sizeof(*workspace->tarjan_index)) ||
        !workspace_allocate_array(workspace,
                                  (void **)&workspace->tarjan_lowlink,
                                  node_capacity,
                                  sizeof(*workspace->tarjan_lowlink)) ||
        !workspace_allocate_array(workspace,
                                  (void **)&workspace->tarjan_stack,
                                  node_capacity,
                                  sizeof(*workspace->tarjan_stack)) ||
        !workspace_allocate_array(workspace, (void **)&workspace->queue,
                                  node_capacity, sizeof(*workspace->queue)) ||
        !workspace_allocate_array(workspace,
                                  (void **)&workspace->reverse_head,
                                  node_capacity,
                                  sizeof(*workspace->reverse_head)) ||
        !workspace_allocate_array(workspace,
                                  (void **)&workspace->reverse_next,
                                  edge_capacity,
                                  sizeof(*workspace->reverse_next)) ||
        !workspace_allocate_array(workspace, (void **)&workspace->out_begin,
                                  node_capacity + 1U,
                                  sizeof(*workspace->out_begin)) ||
        !workspace_allocate_array(workspace, (void **)&workspace->dfs_nodes,
                                  node_capacity,
                                  sizeof(*workspace->dfs_nodes)) ||
        !workspace_allocate_array(workspace, (void **)&workspace->dfs_edges,
                                  node_capacity,
                                  sizeof(*workspace->dfs_edges))) {
        lswg_workspace_destroy(workspace);
        return LSWG_STATUS_OUT_OF_MEMORY;
    }
    return LSWG_STATUS_OK;
}

void
lswg_workspace_destroy(lswg_workspace_t *workspace)
{
    if (workspace == NULL) {
        return;
    }
    if (workspace->allocator.deallocate != NULL) {
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->dfs_edges);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->dfs_nodes);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->out_begin);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->reverse_next);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->reverse_head);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->queue);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->tarjan_stack);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->tarjan_lowlink);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->tarjan_index);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->on_stack);
        workspace->allocator.deallocate(workspace->allocator.context,
                                        workspace->escapable);
    }
    memset(workspace, 0, sizeof(*workspace));
}

static int
compare_ref(lswg_node_ref_t left, lswg_node_ref_t right)
{
    if (left.kind != right.kind) {
        return left.kind < right.kind ? -1 : 1;
    }
    if (left.identity.primary != right.identity.primary) {
        return left.identity.primary < right.identity.primary ? -1 : 1;
    }
    if (left.identity.generation != right.identity.generation) {
        return left.identity.generation < right.identity.generation ? -1 : 1;
    }
    if (left.identity.auxiliary != right.identity.auxiliary) {
        return left.identity.auxiliary < right.identity.auxiliary ? -1 : 1;
    }
    return 0;
}

static bool
node_is_seed(const lswg_node_t *node)
{
    const uint32_t seed_flags =
        LSWG_NODE_RUNNABLE | LSWG_NODE_FUTURE_TIMER |
        LSWG_NODE_LIVE_BACKEND | LSWG_NODE_RUNNING_JOB |
        LSWG_NODE_UNCANCELLED_TOKEN | LSWG_NODE_READY_NOW |
        LSWG_NODE_EXTERNAL_OPEN | LSWG_NODE_TERMINAL_PROGRESS;

    return (node->desc.semantic_flags & seed_flags) != 0U;
}

static void
prepare_adjacency(const lswg_graph_t *graph, lswg_workspace_t *workspace)
{
    size_t index;

    memset(workspace->out_begin, 0,
           (graph->node_count + 1U) * sizeof(*workspace->out_begin));
    for (index = 0U; index < graph->edge_count; ++index) {
        workspace->out_begin[graph->edges[index].source_index + 1U] += 1U;
    }
    for (index = 1U; index <= graph->node_count; ++index) {
        workspace->out_begin[index] += workspace->out_begin[index - 1U];
    }

    for (index = 0U; index < graph->node_count; ++index) {
        workspace->reverse_head[index] = SIZE_MAX;
    }
    for (index = 0U; index < graph->edge_count; ++index) {
        const size_t target = graph->edges[index].target_index;

        workspace->reverse_next[index] = workspace->reverse_head[target];
        workspace->reverse_head[target] = index;
    }
}

static void
propagate_escapability(const lswg_graph_t *graph,
                      lswg_workspace_t *workspace)
{
    size_t head = 0U;
    size_t tail = 0U;
    size_t index;

    memset(workspace->escapable, 0,
           graph->node_count * sizeof(*workspace->escapable));
    for (index = 0U; index < graph->node_count; ++index) {
        if (node_is_seed(&graph->nodes[index])) {
            workspace->escapable[index] |= LSWG_WORK_ESCAPABLE;
            workspace->queue[tail++] = index;
        }
    }
    while (head < tail) {
        size_t reverse_edge = workspace->reverse_head[workspace->queue[head++]];

        while (reverse_edge != SIZE_MAX) {
            const lswg_edge_t *edge = &graph->edges[reverse_edge];
            const size_t source = edge->source_index;

            if ((edge->desc.flags & LSWG_EDGE_INCONSISTENT) == 0U &&
                (workspace->escapable[source] & LSWG_WORK_ESCAPABLE) == 0U) {
                workspace->escapable[source] |= LSWG_WORK_ESCAPABLE;
                workspace->queue[tail++] = source;
            }
            reverse_edge = workspace->reverse_next[reverse_edge];
        }
    }
}

static void
mark_task_reachable_nodes(const lswg_graph_t *graph,
                          lswg_workspace_t *workspace)
{
    size_t head = 0U;
    size_t tail = 0U;
    size_t index;

    for (index = 0U; index < graph->node_count; ++index) {
        if (graph->nodes[index].desc.ref.kind == LSWG_NODE_TASK) {
            workspace->escapable[index] |= LSWG_WORK_RELEVANT;
            workspace->queue[tail++] = index;
        }
    }
    while (head < tail) {
        const size_t source = workspace->queue[head++];
        size_t edge_index;

        for (edge_index = workspace->out_begin[source];
             edge_index < workspace->out_begin[source + 1U]; ++edge_index) {
            const size_t target = graph->edges[edge_index].target_index;

            if ((workspace->escapable[target] & LSWG_WORK_RELEVANT) == 0U) {
                workspace->escapable[target] |= LSWG_WORK_RELEVANT;
                workspace->queue[tail++] = target;
            }
        }
    }
}

static bool
node_is_closed_relevant(const lswg_workspace_t *workspace, size_t index)
{
    return (workspace->escapable[index] & LSWG_WORK_RELEVANT) != 0U &&
           (workspace->escapable[index] & LSWG_WORK_ESCAPABLE) == 0U;
}

static void
result_add_member(lswg_result_t *result, lswg_node_ref_t member)
{
    size_t position;

    if (result->member_count < LSWG_MAX_REPORT_MEMBERS) {
        position = result->member_count;
        while (position > 0U &&
               compare_ref(member, result->members[position - 1U]) < 0) {
            result->members[position] = result->members[position - 1U];
            position -= 1U;
        }
        result->members[position] = member;
        result->member_count += 1U;
        return;
    }
    if (compare_ref(member, result->members[result->member_count - 1U]) < 0) {
        position = result->member_count - 1U;
        while (position > 0U &&
               compare_ref(member, result->members[position - 1U]) < 0) {
            result->members[position] = result->members[position - 1U];
            position -= 1U;
        }
        result->members[position] = member;
    }
}

static void
result_set_single(lswg_result_t *result, lswg_verdict_t verdict,
                  lswg_node_ref_t member)
{
    result->verdict = verdict;
    result->member_count = 1U;
    result->total_member_count = 1U;
    result->members_truncated = false;
    result->members[0] = member;
}

static bool
find_flagged_relevant(const lswg_graph_t *graph,
                      const lswg_workspace_t *workspace, uint32_t flag,
                      size_t *index_out)
{
    size_t index;

    for (index = 0U; index < graph->node_count; ++index) {
        if ((workspace->escapable[index] & LSWG_WORK_RELEVANT) != 0U &&
            (graph->nodes[index].desc.semantic_flags & flag) != 0U) {
            *index_out = index;
            return true;
        }
    }
    return false;
}

static bool
node_has_self_loop(const lswg_graph_t *graph,
                   const lswg_workspace_t *workspace, size_t node_index)
{
    size_t edge_index;

    for (edge_index = workspace->out_begin[node_index];
         edge_index < workspace->out_begin[node_index + 1U]; ++edge_index) {
        if (graph->edges[edge_index].target_index == node_index) {
            return true;
        }
    }
    return false;
}

static void
record_cycle(const lswg_graph_t *graph, lswg_workspace_t *workspace,
             size_t member_count, size_t minimum_index,
             size_t *best_minimum_index, lswg_result_t *result)
{
    size_t index;

    if (minimum_index >= *best_minimum_index) {
        return;
    }
    *best_minimum_index = minimum_index;
    result->verdict = LSWG_VERDICT_PROVEN_CYCLE;
    result->member_count = 0U;
    result->total_member_count = member_count;
    result->members_truncated = member_count > LSWG_MAX_REPORT_MEMBERS;
    for (index = 0U; index < member_count; ++index) {
        result_add_member(result,
                          graph->nodes[workspace->queue[index]].desc.ref);
    }
}

static bool
find_closed_cycle(const lswg_graph_t *graph, lswg_workspace_t *workspace,
                  lswg_result_t *result)
{
    uint32_t next_index = 0U;
    size_t tarjan_top = 0U;
    size_t best_minimum_index = SIZE_MAX;
    size_t start;

    memset(workspace->tarjan_index, 0xff,
           graph->node_count * sizeof(*workspace->tarjan_index));
    memset(workspace->tarjan_lowlink, 0,
           graph->node_count * sizeof(*workspace->tarjan_lowlink));
    memset(workspace->on_stack, 0,
           graph->node_count * sizeof(*workspace->on_stack));

    for (start = 0U; start < graph->node_count; ++start) {
        size_t depth;

        if (!node_is_closed_relevant(workspace, start) ||
            workspace->tarjan_index[start] != UINT32_MAX) {
            continue;
        }
        depth = 1U;
        workspace->dfs_nodes[0] = start;
        workspace->dfs_edges[0] = workspace->out_begin[start];

        while (depth != 0U) {
            const size_t frame = depth - 1U;
            const size_t node_index = workspace->dfs_nodes[frame];
            size_t edge_index;

            if (workspace->tarjan_index[node_index] == UINT32_MAX) {
                workspace->tarjan_index[node_index] = next_index;
                workspace->tarjan_lowlink[node_index] = next_index;
                next_index += 1U;
                workspace->tarjan_stack[tarjan_top++] = node_index;
                workspace->on_stack[node_index] = 1U;
            }

            edge_index = workspace->dfs_edges[frame];
            while (edge_index < workspace->out_begin[node_index + 1U] &&
                   !node_is_closed_relevant(
                       workspace, graph->edges[edge_index].target_index)) {
                edge_index += 1U;
            }
            workspace->dfs_edges[frame] = edge_index;
            if (edge_index < workspace->out_begin[node_index + 1U]) {
                const size_t target = graph->edges[edge_index].target_index;

                workspace->dfs_edges[frame] = edge_index + 1U;
                if (workspace->tarjan_index[target] == UINT32_MAX) {
                    workspace->dfs_nodes[depth] = target;
                    workspace->dfs_edges[depth] =
                        workspace->out_begin[target];
                    depth += 1U;
                    continue;
                }
                if (workspace->on_stack[target] != 0U &&
                    workspace->tarjan_index[target] <
                        workspace->tarjan_lowlink[node_index]) {
                    workspace->tarjan_lowlink[node_index] =
                        workspace->tarjan_index[target];
                }
                continue;
            }

            if (workspace->tarjan_lowlink[node_index] ==
                workspace->tarjan_index[node_index]) {
                size_t member_count = 0U;
                size_t minimum_index = SIZE_MAX;
                size_t member;

                do {
                    member = workspace->tarjan_stack[--tarjan_top];
                    workspace->on_stack[member] = 0U;
                    workspace->queue[member_count++] = member;
                    if (member < minimum_index) {
                        minimum_index = member;
                    }
                } while (member != node_index);

                if (member_count > 1U ||
                    node_has_self_loop(graph, workspace, node_index)) {
                    record_cycle(graph, workspace, member_count,
                                 minimum_index, &best_minimum_index, result);
                }
            }

            depth -= 1U;
            if (depth != 0U) {
                const size_t parent = workspace->dfs_nodes[depth - 1U];

                if (workspace->tarjan_lowlink[node_index] <
                    workspace->tarjan_lowlink[parent]) {
                    workspace->tarjan_lowlink[parent] =
                        workspace->tarjan_lowlink[node_index];
                }
            }
        }
    }
    return best_minimum_index != SIZE_MAX;
}

static bool
find_orphan_sink(const lswg_graph_t *graph,
                 const lswg_workspace_t *workspace, size_t *orphan_out)
{
    size_t node_index;

    for (node_index = 0U; node_index < graph->node_count; ++node_index) {
        size_t edge_index;
        bool has_closed_dependency = false;

        if (!node_is_closed_relevant(workspace, node_index)) {
            continue;
        }
        for (edge_index = workspace->out_begin[node_index];
             edge_index < workspace->out_begin[node_index + 1U]; ++edge_index) {
            if (node_is_closed_relevant(
                    workspace, graph->edges[edge_index].target_index)) {
                has_closed_dependency = true;
                break;
            }
        }
        if (!has_closed_dependency) {
            *orphan_out = node_index;
            return true;
        }
    }
    return false;
}

lswg_status_t
lswg_solve(const lswg_graph_t *graph, lswg_workspace_t *workspace,
           lswg_result_t *result)
{
    size_t index;
    size_t closed_task_count = 0U;
    size_t task_count = 0U;
    size_t evidence_index;
    uint32_t incomplete_reasons;

    if (graph == NULL || workspace == NULL || result == NULL) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    if (!graph->finalized) {
        return LSWG_STATUS_NOT_FINALIZED;
    }
    incomplete_reasons = graph->incomplete_reasons;
    if (graph->node_count > workspace->node_capacity ||
        graph->edge_count > workspace->edge_capacity) {
        incomplete_reasons |= LSWG_INCOMPLETE_WORKSPACE;
    }
    for (index = 0U; index < graph->edge_count; ++index) {
        if ((graph->edges[index].desc.flags & LSWG_EDGE_INCONSISTENT) != 0U) {
            incomplete_reasons |= LSWG_INCOMPLETE_MALFORMED;
        }
    }
    if (incomplete_reasons != LSWG_INCOMPLETE_NONE) {
        result->verdict = LSWG_VERDICT_INCOMPLETE;
        result->incomplete_reasons = incomplete_reasons;
        return LSWG_STATUS_OK;
    }

    prepare_adjacency(graph, workspace);
    propagate_escapability(graph, workspace);
    mark_task_reachable_nodes(graph, workspace);

    if (find_flagged_relevant(graph, workspace,
                              LSWG_NODE_MATCHABLE_STUCK,
                              &evidence_index)) {
        result_set_single(result, LSWG_VERDICT_MATCHABLE_LOST_WAKE,
                          graph->nodes[evidence_index].desc.ref);
        return LSWG_STATUS_OK;
    }
    if (find_flagged_relevant(graph, workspace, LSWG_NODE_OVERDUE_STUCK,
                              &evidence_index)) {
        result_set_single(result, LSWG_VERDICT_OVERDUE_SOURCE,
                          graph->nodes[evidence_index].desc.ref);
        return LSWG_STATUS_OK;
    }

    for (index = 0U; index < graph->node_count; ++index) {
        if (graph->nodes[index].desc.ref.kind == LSWG_NODE_TASK) {
            task_count += 1U;
            if ((workspace->escapable[index] & LSWG_WORK_ESCAPABLE) == 0U) {
                closed_task_count += 1U;
            }
        }
    }
    if (task_count == 0U) {
        result->verdict = LSWG_VERDICT_NONE;
        return LSWG_STATUS_OK;
    }
    if (closed_task_count == 0U) {
        result->verdict = LSWG_VERDICT_OPEN;
        return LSWG_STATUS_OK;
    }
    if (find_closed_cycle(graph, workspace, result)) {
        return LSWG_STATUS_OK;
    }
    if (find_orphan_sink(graph, workspace, &evidence_index)) {
        result_set_single(result, LSWG_VERDICT_PROVEN_ORPHAN,
                          graph->nodes[evidence_index].desc.ref);
        return LSWG_STATUS_OK;
    }

    result->verdict = LSWG_VERDICT_INCOMPLETE;
    result->incomplete_reasons = LSWG_INCOMPLETE_MALFORMED;
    return LSWG_STATUS_OK;
}
