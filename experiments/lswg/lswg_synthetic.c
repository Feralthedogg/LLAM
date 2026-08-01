/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lswg_graph.h"

#include <stdlib.h>
#include <string.h>

static void *
default_allocate(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}

static void
default_deallocate(void *context, void *pointer)
{
    (void)context;
    free(pointer);
}

static lswg_status_t
normalize_allocator(const lswg_allocator_t *requested,
                    lswg_allocator_t *normalized)
{
    if (normalized == NULL) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    if (requested == NULL) {
        normalized->context = NULL;
        normalized->allocate = default_allocate;
        normalized->deallocate = default_deallocate;
        return LSWG_STATUS_OK;
    }
    if (requested->allocate == NULL || requested->deallocate == NULL) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    *normalized = *requested;
    return LSWG_STATUS_OK;
}

static bool
checked_allocation_size(size_t count, size_t element_size, size_t *size_out)
{
    if (size_out == NULL ||
        (element_size != 0U && count > SIZE_MAX / element_size)) {
        return false;
    }
    *size_out = count * element_size;
    return true;
}

static bool
identity_equal(lswg_node_ref_t left, lswg_node_ref_t right)
{
    return left.kind == right.kind &&
           left.identity.primary == right.identity.primary &&
           left.identity.generation == right.identity.generation &&
           left.identity.auxiliary == right.identity.auxiliary;
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

static int
compare_nodes(const void *left_pointer, const void *right_pointer)
{
    const lswg_node_t *left = left_pointer;
    const lswg_node_t *right = right_pointer;

    return compare_ref(left->desc.ref, right->desc.ref);
}

static int
compare_edges(const void *left_pointer, const void *right_pointer)
{
    const lswg_edge_t *left = left_pointer;
    const lswg_edge_t *right = right_pointer;

    if (left->source_index != right->source_index) {
        return left->source_index < right->source_index ? -1 : 1;
    }
    if (left->target_index != right->target_index) {
        return left->target_index < right->target_index ? -1 : 1;
    }
    if (left->desc.kind != right->desc.kind) {
        return left->desc.kind < right->desc.kind ? -1 : 1;
    }
    if (left->desc.flags != right->desc.flags) {
        return left->desc.flags < right->desc.flags ? -1 : 1;
    }
    return 0;
}

static bool
find_node(const lswg_graph_t *graph, lswg_node_ref_t ref, size_t *index_out)
{
    size_t begin = 0U;
    size_t end = graph->node_count;

    while (begin < end) {
        const size_t middle = begin + (end - begin) / 2U;
        const int comparison = compare_ref(graph->nodes[middle].desc.ref, ref);

        if (comparison < 0) {
            begin = middle + 1U;
        } else {
            end = middle;
        }
    }
    if (begin >= graph->node_count ||
        !identity_equal(graph->nodes[begin].desc.ref, ref)) {
        return false;
    }
    *index_out = begin;
    return true;
}

lswg_status_t
lswg_graph_init(lswg_graph_t *graph, size_t node_capacity,
                size_t edge_capacity, const lswg_allocator_t *allocator)
{
    lswg_allocator_t selected;
    size_t node_bytes;
    size_t edge_bytes;

    if (graph == NULL ||
        normalize_allocator(allocator, &selected) != LSWG_STATUS_OK ||
        !checked_allocation_size(node_capacity, sizeof(*graph->nodes),
                                 &node_bytes) ||
        !checked_allocation_size(edge_capacity, sizeof(*graph->edges),
                                 &edge_bytes)) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }

    memset(graph, 0, sizeof(*graph));
    graph->allocator = selected;
    graph->node_capacity = node_capacity;
    graph->edge_capacity = edge_capacity;
    if (node_bytes != 0U) {
        graph->nodes = selected.allocate(selected.context, node_bytes);
        if (graph->nodes == NULL) {
            memset(graph, 0, sizeof(*graph));
            return LSWG_STATUS_OUT_OF_MEMORY;
        }
    }
    if (edge_bytes != 0U) {
        graph->edges = selected.allocate(selected.context, edge_bytes);
        if (graph->edges == NULL) {
            selected.deallocate(selected.context, graph->nodes);
            memset(graph, 0, sizeof(*graph));
            return LSWG_STATUS_OUT_OF_MEMORY;
        }
    }
    return LSWG_STATUS_OK;
}

void
lswg_graph_destroy(lswg_graph_t *graph)
{
    if (graph == NULL) {
        return;
    }
    if (graph->allocator.deallocate != NULL) {
        graph->allocator.deallocate(graph->allocator.context, graph->edges);
        graph->allocator.deallocate(graph->allocator.context, graph->nodes);
    }
    memset(graph, 0, sizeof(*graph));
}

void
lswg_graph_reset(lswg_graph_t *graph)
{
    if (graph == NULL) {
        return;
    }
    graph->node_count = 0U;
    graph->edge_count = 0U;
    graph->incomplete_reasons = LSWG_INCOMPLETE_NONE;
    graph->capture_seq = 0U;
    graph->finalized = false;
}

lswg_status_t
lswg_graph_add_node(lswg_graph_t *graph, const lswg_node_desc_t *node)
{
    if (graph == NULL || node == NULL || graph->finalized) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    if (node->ref.kind < LSWG_NODE_TASK ||
        node->ref.kind >= LSWG_NODE_KIND_COUNT) {
        return LSWG_STATUS_INVALID_KIND;
    }
    if (graph->node_count >= graph->node_capacity) {
        return LSWG_STATUS_CAPACITY;
    }
    graph->nodes[graph->node_count].desc = *node;
    graph->node_count += 1U;
    return LSWG_STATUS_OK;
}

lswg_status_t
lswg_graph_add_edge(lswg_graph_t *graph, const lswg_edge_desc_t *edge)
{
    if (graph == NULL || edge == NULL || graph->finalized) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    if (edge->source.kind < LSWG_NODE_TASK ||
        edge->source.kind >= LSWG_NODE_KIND_COUNT ||
        edge->target.kind < LSWG_NODE_TASK ||
        edge->target.kind >= LSWG_NODE_KIND_COUNT ||
        edge->kind < LSWG_EDGE_TASK_WAITS_MUTEX ||
        edge->kind >= LSWG_EDGE_KIND_COUNT) {
        return LSWG_STATUS_INVALID_KIND;
    }
    if (graph->edge_count >= graph->edge_capacity) {
        return LSWG_STATUS_CAPACITY;
    }
    graph->edges[graph->edge_count].desc = *edge;
    graph->edges[graph->edge_count].source_index = SIZE_MAX;
    graph->edges[graph->edge_count].target_index = SIZE_MAX;
    graph->edge_count += 1U;
    return LSWG_STATUS_OK;
}

void
lswg_graph_mark_incomplete(lswg_graph_t *graph, uint32_t reasons)
{
    if (graph != NULL) {
        graph->incomplete_reasons |= reasons;
    }
}

static lswg_status_t
validate_edge_shape(const lswg_graph_t *graph, const lswg_edge_t *edge)
{
    const lswg_node_kind_t source_kind =
        graph->nodes[edge->source_index].desc.ref.kind;
    const lswg_node_kind_t target_kind =
        graph->nodes[edge->target_index].desc.ref.kind;

    switch (edge->desc.kind) {
    case LSWG_EDGE_TASK_WAITS_MUTEX:
        return source_kind == LSWG_NODE_TASK && target_kind == LSWG_NODE_MUTEX
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_MUTEX_OWNED_BY_TASK:
        return source_kind == LSWG_NODE_MUTEX && target_kind == LSWG_NODE_TASK
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_TASK_WAITS_JOIN:
        return source_kind == LSWG_NODE_TASK && target_kind == LSWG_NODE_TASK
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_TASK_WAITS_CHANNEL_SEND:
    case LSWG_EDGE_TASK_WAITS_CHANNEL_RECV:
        return source_kind == LSWG_NODE_TASK &&
                       target_kind == LSWG_NODE_CHANNEL
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_TASK_WAITS_SELECT:
        return source_kind == LSWG_NODE_TASK && target_kind == LSWG_NODE_SELECT
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_MALFORMED_SELECT;
    case LSWG_EDGE_SELECT_ALTERNATIVE:
        return source_kind == LSWG_NODE_SELECT &&
                       (edge->desc.flags & LSWG_EDGE_OR_ALTERNATIVE) != 0U
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_MALFORMED_SELECT;
    case LSWG_EDGE_CHANNEL_MATCHED_BY_TASK:
        return source_kind == LSWG_NODE_CHANNEL && target_kind == LSWG_NODE_TASK
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_TASK_WAITS_COND:
        return source_kind == LSWG_NODE_TASK && target_kind == LSWG_NODE_COND
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_TASK_WAITS_IO:
        return source_kind == LSWG_NODE_TASK && target_kind == LSWG_NODE_IO_REQ
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_IO_OWNED_BY_NODE:
        return source_kind == LSWG_NODE_IO_REQ ? LSWG_STATUS_OK
                                                : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_TASK_WAITS_TIMER:
        return source_kind == LSWG_NODE_TASK && target_kind == LSWG_NODE_TIMER
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_TASK_WAITS_BLOCK_JOB:
        return source_kind == LSWG_NODE_TASK &&
                       target_kind == LSWG_NODE_BLOCK_JOB
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_TASK_CAN_CANCEL:
        return source_kind == LSWG_NODE_TASK &&
                       target_kind == LSWG_NODE_CANCEL_TOKEN
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_RESOURCE_EXTERNAL_SIGNAL:
        return (source_kind == LSWG_NODE_COND ||
                source_kind == LSWG_NODE_CHANNEL) &&
                       target_kind == LSWG_NODE_EXTERNAL_SOURCE
                   ? LSWG_STATUS_OK
                   : LSWG_STATUS_INVALID_KIND;
    case LSWG_EDGE_KIND_COUNT:
        return LSWG_STATUS_INVALID_KIND;
    }
    return LSWG_STATUS_INVALID_KIND;
}

lswg_status_t
lswg_graph_finalize(lswg_graph_t *graph)
{
    size_t index;
    size_t edge_cursor = 0U;

    if (graph == NULL || graph->finalized) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    if (graph->node_count > 1U) {
        qsort(graph->nodes, graph->node_count, sizeof(*graph->nodes),
              compare_nodes);
    }
    for (index = 1U; index < graph->node_count; ++index) {
        if (identity_equal(graph->nodes[index - 1U].desc.ref,
                           graph->nodes[index].desc.ref)) {
            return LSWG_STATUS_DUPLICATE_IDENTITY;
        }
    }
    for (index = 0U; index < graph->edge_count; ++index) {
        lswg_status_t validation_status;

        if (!find_node(graph, graph->edges[index].desc.source,
                       &graph->edges[index].source_index) ||
            !find_node(graph, graph->edges[index].desc.target,
                       &graph->edges[index].target_index)) {
            return LSWG_STATUS_MISSING_ENDPOINT;
        }
        validation_status = validate_edge_shape(graph, &graph->edges[index]);
        if (validation_status != LSWG_STATUS_OK) {
            return validation_status;
        }
    }
    if (graph->edge_count > 1U) {
        qsort(graph->edges, graph->edge_count, sizeof(*graph->edges),
              compare_edges);
    }
    for (index = 0U; index < graph->node_count; ++index) {
        size_t edge_index;
        bool has_alternative = false;

        while (edge_cursor < graph->edge_count &&
               graph->edges[edge_cursor].source_index < index) {
            edge_cursor += 1U;
        }
        if (graph->nodes[index].desc.ref.kind != LSWG_NODE_SELECT) {
            continue;
        }
        for (edge_index = edge_cursor;
             edge_index < graph->edge_count &&
             graph->edges[edge_index].source_index == index;
             ++edge_index) {
            if (graph->edges[edge_index].desc.kind ==
                    LSWG_EDGE_SELECT_ALTERNATIVE) {
                has_alternative = true;
                break;
            }
        }
        if (!has_alternative) {
            return LSWG_STATUS_MALFORMED_SELECT;
        }
    }
    graph->finalized = true;
    return LSWG_STATUS_OK;
}

static lswg_node_ref_t
synthetic_ref(lswg_node_kind_t kind, uint64_t primary, uint64_t generation,
              uint64_t auxiliary)
{
    lswg_node_ref_t ref;

    ref.kind = kind;
    ref.identity.primary = primary;
    ref.identity.generation = generation;
    ref.identity.auxiliary = auxiliary;
    return ref;
}

static lswg_status_t
synthetic_add_node(lswg_graph_t *graph, lswg_node_ref_t ref, uint32_t flags)
{
    lswg_node_desc_t node;

    memset(&node, 0, sizeof(node));
    node.ref = ref;
    node.semantic_flags = flags;
    return lswg_graph_add_node(graph, &node);
}

static lswg_status_t
synthetic_add_edge(lswg_graph_t *graph, lswg_node_ref_t source,
                   lswg_node_ref_t target, lswg_edge_kind_t kind,
                   uint32_t flags)
{
    lswg_edge_desc_t edge;

    edge.source = source;
    edge.target = target;
    edge.kind = kind;
    edge.flags = flags;
    return lswg_graph_add_edge(graph, &edge);
}

static lswg_status_t
populate_long_chain_open(lswg_graph_t *graph, size_t node_count)
{
    size_t index;

    for (index = 0U; index < node_count; ++index) {
        const uint32_t flags =
            index + 1U == node_count ? LSWG_NODE_RUNNABLE : 0U;
        const lswg_status_t status = synthetic_add_node(
            graph,
            synthetic_ref(LSWG_NODE_TASK, (uint64_t)index + 1U, 1U, 1U),
            flags);

        if (status != LSWG_STATUS_OK) {
            return status;
        }
    }
    for (index = 0U; index + 1U < node_count; ++index) {
        const lswg_status_t status = synthetic_add_edge(
            graph,
            synthetic_ref(LSWG_NODE_TASK, (uint64_t)index + 1U, 1U, 1U),
            synthetic_ref(LSWG_NODE_TASK, (uint64_t)index + 2U, 1U, 1U),
            LSWG_EDGE_TASK_WAITS_JOIN,
            LSWG_EDGE_AND_REQUIRED | LSWG_EDGE_GENERATION_STABLE);

        if (status != LSWG_STATUS_OK) {
            return status;
        }
    }
    return LSWG_STATUS_OK;
}

static lswg_status_t
populate_many_sccs(lswg_graph_t *graph, size_t node_count)
{
    size_t index;

    for (index = 0U; index < node_count; ++index) {
        const lswg_status_t status = synthetic_add_node(
            graph,
            synthetic_ref(LSWG_NODE_TASK, (uint64_t)index + 1U, 2U, 1U),
            0U);

        if (status != LSWG_STATUS_OK) {
            return status;
        }
    }
    for (index = 0U; index + 1U < node_count; index += 2U) {
        const lswg_node_ref_t first =
            synthetic_ref(LSWG_NODE_TASK, (uint64_t)index + 1U, 2U, 1U);
        const lswg_node_ref_t second =
            synthetic_ref(LSWG_NODE_TASK, (uint64_t)index + 2U, 2U, 1U);
        lswg_status_t status = synthetic_add_edge(
            graph, first, second, LSWG_EDGE_TASK_WAITS_JOIN,
            LSWG_EDGE_AND_REQUIRED | LSWG_EDGE_GENERATION_STABLE);

        if (status == LSWG_STATUS_OK) {
            status = synthetic_add_edge(
                graph, second, first, LSWG_EDGE_TASK_WAITS_JOIN,
                LSWG_EDGE_AND_REQUIRED | LSWG_EDGE_GENERATION_STABLE);
        }
        if (status != LSWG_STATUS_OK) {
            return status;
        }
    }
    if ((node_count & 1U) != 0U) {
        const lswg_node_ref_t last = synthetic_ref(
            LSWG_NODE_TASK, (uint64_t)node_count, 2U, 1U);

        return synthetic_add_edge(
            graph, last, last, LSWG_EDGE_TASK_WAITS_JOIN,
            LSWG_EDGE_AND_REQUIRED | LSWG_EDGE_GENERATION_STABLE);
    }
    return LSWG_STATUS_OK;
}

static lswg_status_t
populate_or_fanout_open(lswg_graph_t *graph, size_t node_count)
{
    const lswg_node_ref_t task =
        synthetic_ref(LSWG_NODE_TASK, 1U, 3U, 1U);
    const lswg_node_ref_t select =
        synthetic_ref(LSWG_NODE_SELECT, 1U, 3U, 0U);
    size_t index;
    lswg_status_t status;

    status = synthetic_add_node(graph, task, 0U);
    if (status == LSWG_STATUS_OK) {
        status = synthetic_add_node(graph, select, 0U);
    }
    if (status == LSWG_STATUS_OK) {
        status = synthetic_add_edge(
            graph, task, select, LSWG_EDGE_TASK_WAITS_SELECT,
            LSWG_EDGE_AND_REQUIRED | LSWG_EDGE_GENERATION_STABLE);
    }
    if (status != LSWG_STATUS_OK) {
        return status;
    }

    for (index = 2U; index < node_count; ++index) {
        const lswg_node_ref_t source = synthetic_ref(
            LSWG_NODE_EXTERNAL_SOURCE, (uint64_t)index - 1U, 1U, 0U);

        status = synthetic_add_node(graph, source, LSWG_NODE_EXTERNAL_OPEN);
        if (status == LSWG_STATUS_OK) {
            status = synthetic_add_edge(
                graph, select, source, LSWG_EDGE_SELECT_ALTERNATIVE,
                LSWG_EDGE_OR_ALTERNATIVE | LSWG_EDGE_EXTERNAL_OPEN |
                    LSWG_EDGE_GENERATION_STABLE);
        }
        if (status != LSWG_STATUS_OK) {
            return status;
        }
    }
    return LSWG_STATUS_OK;
}

static lswg_status_t
populate_mostly_open_with_orphan(lswg_graph_t *graph, size_t node_count)
{
    size_t index;

    for (index = 0U; index < node_count; ++index) {
        const uint32_t flags =
            index + 1U == node_count ? LSWG_NODE_RUNNABLE : 0U;
        const lswg_status_t status = synthetic_add_node(
            graph,
            synthetic_ref(LSWG_NODE_TASK, (uint64_t)index + 1U, 4U, 1U),
            flags);

        if (status != LSWG_STATUS_OK) {
            return status;
        }
    }
    for (index = 1U; index + 1U < node_count; ++index) {
        const lswg_status_t status = synthetic_add_edge(
            graph,
            synthetic_ref(LSWG_NODE_TASK, (uint64_t)index + 1U, 4U, 1U),
            synthetic_ref(LSWG_NODE_TASK, (uint64_t)node_count, 4U, 1U),
            LSWG_EDGE_TASK_WAITS_JOIN,
            LSWG_EDGE_AND_REQUIRED | LSWG_EDGE_GENERATION_STABLE);

        if (status != LSWG_STATUS_OK) {
            return status;
        }
    }
    return LSWG_STATUS_OK;
}

lswg_status_t
lswg_synthetic_populate(lswg_graph_t *graph, size_t node_count,
                        lswg_synthetic_profile_t profile)
{
    size_t required_edges;
    lswg_status_t status;

    if (graph == NULL || profile < LSWG_SYNTHETIC_LONG_CHAIN_OPEN ||
        profile > LSWG_SYNTHETIC_MOSTLY_OPEN_WITH_ORPHAN) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    switch (profile) {
    case LSWG_SYNTHETIC_LONG_CHAIN_OPEN:
        if (node_count < 2U) {
            return LSWG_STATUS_INVALID_ARGUMENT;
        }
        required_edges = node_count - 1U;
        break;
    case LSWG_SYNTHETIC_MANY_SCCS:
        if (node_count == 0U) {
            return LSWG_STATUS_INVALID_ARGUMENT;
        }
        required_edges = node_count;
        break;
    case LSWG_SYNTHETIC_OR_FANOUT_OPEN:
        if (node_count < 3U) {
            return LSWG_STATUS_INVALID_ARGUMENT;
        }
        required_edges = node_count - 1U;
        break;
    case LSWG_SYNTHETIC_MOSTLY_OPEN_WITH_ORPHAN:
        if (node_count < 2U) {
            return LSWG_STATUS_INVALID_ARGUMENT;
        }
        required_edges = node_count - 2U;
        break;
    default:
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    if (node_count > graph->node_capacity ||
        required_edges > graph->edge_capacity) {
        return LSWG_STATUS_CAPACITY;
    }

    lswg_graph_reset(graph);
    switch (profile) {
    case LSWG_SYNTHETIC_LONG_CHAIN_OPEN:
        status = populate_long_chain_open(graph, node_count);
        break;
    case LSWG_SYNTHETIC_MANY_SCCS:
        status = populate_many_sccs(graph, node_count);
        break;
    case LSWG_SYNTHETIC_OR_FANOUT_OPEN:
        status = populate_or_fanout_open(graph, node_count);
        break;
    case LSWG_SYNTHETIC_MOSTLY_OPEN_WITH_ORPHAN:
        status = populate_mostly_open_with_orphan(graph, node_count);
        break;
    default:
        status = LSWG_STATUS_INVALID_ARGUMENT;
        break;
    }
    if (status != LSWG_STATUS_OK) {
        lswg_graph_reset(graph);
    }
    return status;
}
