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

static bool
select_has_valid_alternative(const lswg_graph_t *graph, size_t select_index)
{
    size_t edge_index;
    bool has_alternative = false;

    for (edge_index = 0U; edge_index < graph->edge_count; ++edge_index) {
        const lswg_edge_t *current = &graph->edges[edge_index];

        if (current->desc.kind == LSWG_EDGE_SELECT_ALTERNATIVE) {
            if (graph->nodes[current->source_index].desc.ref.kind !=
                    LSWG_NODE_SELECT ||
                (current->desc.flags & LSWG_EDGE_OR_ALTERNATIVE) == 0U) {
                return false;
            }
            if (current->source_index == select_index) {
                has_alternative = true;
            }
        }
        if (current->desc.kind == LSWG_EDGE_TASK_WAITS_SELECT &&
            graph->nodes[current->target_index].desc.ref.kind !=
                LSWG_NODE_SELECT) {
            return false;
        }
    }
    return has_alternative;
}

lswg_status_t
lswg_graph_finalize(lswg_graph_t *graph)
{
    size_t index;

    if (graph == NULL || graph->finalized) {
        return LSWG_STATUS_INVALID_ARGUMENT;
    }
    qsort(graph->nodes, graph->node_count, sizeof(*graph->nodes),
          compare_nodes);
    for (index = 1U; index < graph->node_count; ++index) {
        if (identity_equal(graph->nodes[index - 1U].desc.ref,
                           graph->nodes[index].desc.ref)) {
            return LSWG_STATUS_DUPLICATE_IDENTITY;
        }
    }
    for (index = 0U; index < graph->edge_count; ++index) {
        if (!find_node(graph, graph->edges[index].desc.source,
                       &graph->edges[index].source_index) ||
            !find_node(graph, graph->edges[index].desc.target,
                       &graph->edges[index].target_index)) {
            return LSWG_STATUS_MISSING_ENDPOINT;
        }
    }
    for (index = 0U; index < graph->node_count; ++index) {
        if (graph->nodes[index].desc.ref.kind == LSWG_NODE_SELECT &&
            !select_has_valid_alternative(graph, index)) {
            return LSWG_STATUS_MALFORMED_SELECT;
        }
    }
    qsort(graph->edges, graph->edge_count, sizeof(*graph->edges),
          compare_edges);
    graph->finalized = true;
    return LSWG_STATUS_OK;
}
