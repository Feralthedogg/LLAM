/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lswg_graph.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_CHECK(condition)                                                   \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return false;                                                       \
        }                                                                       \
    } while (0)

static size_t scale_node_count = 4096U;
static bool emit_scale_timing = false;

static lswg_node_ref_t
ref(lswg_node_kind_t kind, uint64_t primary, uint64_t generation,
    uint64_t auxiliary)
{
    lswg_node_ref_t value;

    value.kind = kind;
    value.identity.primary = primary;
    value.identity.generation = generation;
    value.identity.auxiliary = auxiliary;
    return value;
}

static lswg_node_desc_t
node(lswg_node_ref_t value, uint32_t flags, uint64_t semantic_value,
     uintptr_t raw_address)
{
    lswg_node_desc_t desc;

    desc.ref = value;
    desc.semantic_flags = flags;
    desc.semantic_value = semantic_value;
    desc.raw_address = raw_address;
    return desc;
}

static lswg_edge_desc_t
edge(lswg_node_ref_t source, lswg_node_ref_t target, lswg_edge_kind_t kind,
     uint32_t flags)
{
    lswg_edge_desc_t desc;

    desc.source = source;
    desc.target = target;
    desc.kind = kind;
    desc.flags = flags;
    return desc;
}

static bool
add_node(lswg_graph_t *graph, lswg_node_desc_t desc)
{
    return lswg_graph_add_node(graph, &desc) == LSWG_STATUS_OK;
}

static bool
add_edge(lswg_graph_t *graph, lswg_edge_desc_t desc)
{
    return lswg_graph_add_edge(graph, &desc) == LSWG_STATUS_OK;
}

#ifndef LSWG_GRAPH_ONLY
static bool
solve_graph(lswg_graph_t *graph, lswg_result_t *result)
{
    lswg_workspace_t workspace;
    bool ok;

    TEST_CHECK(lswg_graph_finalize(graph) == LSWG_STATUS_OK);
    TEST_CHECK(lswg_workspace_init(&workspace, graph->node_count,
                                   graph->edge_count, NULL) == LSWG_STATUS_OK);
    ok = lswg_solve(graph, &workspace, result) == LSWG_STATUS_OK;
    lswg_workspace_destroy(&workspace);
    return ok;
}

static bool
test_two_task_mutex_cycle(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task_1 = ref(LSWG_NODE_TASK, 1U, 7U, 2U);
    const lswg_node_ref_t task_2 = ref(LSWG_NODE_TASK, 2U, 9U, 2U);
    const lswg_node_ref_t mutex_1 = ref(LSWG_NODE_MUTEX, 40U, 3U, 0U);
    const lswg_node_ref_t mutex_2 = ref(LSWG_NODE_MUTEX, 41U, 3U, 0U);

    TEST_CHECK(lswg_graph_init(&graph, 4U, 4U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task_1, 0U, 0U, 0x1000U)));
    TEST_CHECK(add_node(&graph, node(task_2, 0U, 0U, 0x2000U)));
    TEST_CHECK(add_node(&graph, node(mutex_1, 0U, 0U, 0x3000U)));
    TEST_CHECK(add_node(&graph, node(mutex_2, 0U, 0U, 0x4000U)));
    TEST_CHECK(add_edge(&graph, edge(task_1, mutex_1,
                                     LSWG_EDGE_TASK_WAITS_MUTEX,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(add_edge(&graph, edge(mutex_1, task_2,
                                     LSWG_EDGE_MUTEX_OWNED_BY_TASK,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(add_edge(&graph, edge(task_2, mutex_2,
                                     LSWG_EDGE_TASK_WAITS_MUTEX,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(add_edge(&graph, edge(mutex_2, task_1,
                                     LSWG_EDGE_MUTEX_OWNED_BY_TASK,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));

    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_PROVEN_CYCLE);
    TEST_CHECK(result.member_count == 4U);
    TEST_CHECK(result.members[0].kind == LSWG_NODE_TASK);
    TEST_CHECK(result.members[0].identity.primary == 1U);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_join_cycle(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task_8 = ref(LSWG_NODE_TASK, 8U, 1U, 4U);
    const lswg_node_ref_t task_9 = ref(LSWG_NODE_TASK, 9U, 1U, 4U);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 2U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task_8, 0U, 0U, 0x8000U)));
    TEST_CHECK(add_node(&graph, node(task_9, 0U, 0U, 0x9000U)));
    TEST_CHECK(add_edge(&graph, edge(task_8, task_9,
                                     LSWG_EDGE_TASK_WAITS_JOIN,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(add_edge(&graph, edge(task_9, task_8,
                                     LSWG_EDGE_TASK_WAITS_JOIN,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_PROVEN_CYCLE);
    TEST_CHECK(result.member_count == 2U);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_open_world_condition(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 20U, 2U, 1U);
    const lswg_node_ref_t cond = ref(LSWG_NODE_COND, 3U, 5U, 0U);
    const lswg_node_ref_t host =
        ref(LSWG_NODE_EXTERNAL_SOURCE, 1U, 1U, 0U);

    TEST_CHECK(lswg_graph_init(&graph, 3U, 2U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(cond, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph,
                        node(host, LSWG_NODE_EXTERNAL_OPEN, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, cond, LSWG_EDGE_TASK_WAITS_COND,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(add_edge(&graph, edge(cond, host,
                                     LSWG_EDGE_RESOURCE_EXTERNAL_SIGNAL,
                                     LSWG_EDGE_EXTERNAL_OPEN |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_OPEN);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_select_future_timer_alternative(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 30U, 3U, 1U);
    const lswg_node_ref_t select = ref(LSWG_NODE_SELECT, 30U, 11U, 0U);
    const lswg_node_ref_t timer = ref(LSWG_NODE_TIMER, 30U, 3U, 9000U);

    TEST_CHECK(lswg_graph_init(&graph, 3U, 2U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(select, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph,
                        node(timer, LSWG_NODE_FUTURE_TIMER, 9000U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, select,
                                     LSWG_EDGE_TASK_WAITS_SELECT,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(add_edge(&graph, edge(select, timer,
                                     LSWG_EDGE_SELECT_ALTERNATIVE,
                                     LSWG_EDGE_OR_ALTERNATIVE |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_OPEN);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_owner_blocked_on_live_io(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t waiter = ref(LSWG_NODE_TASK, 40U, 2U, 1U);
    const lswg_node_ref_t mutex = ref(LSWG_NODE_MUTEX, 4U, 8U, 0U);
    const lswg_node_ref_t owner = ref(LSWG_NODE_TASK, 41U, 4U, 1U);
    const lswg_node_ref_t io = ref(LSWG_NODE_IO_REQ, 90U, 6U, 7U);
    const lswg_node_ref_t backend =
        ref(LSWG_NODE_BACKEND_SOURCE, 7U, 1U, 0U);

    TEST_CHECK(lswg_graph_init(&graph, 5U, 4U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(waiter, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(mutex, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(owner, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(io, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph,
                        node(backend, LSWG_NODE_LIVE_BACKEND, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(waiter, mutex,
                                     LSWG_EDGE_TASK_WAITS_MUTEX,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(add_edge(&graph, edge(mutex, owner,
                                     LSWG_EDGE_MUTEX_OWNED_BY_TASK,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(add_edge(&graph, edge(owner, io, LSWG_EDGE_TASK_WAITS_IO,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(add_edge(&graph, edge(io, backend,
                                     LSWG_EDGE_IO_OWNED_BY_NODE,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_OPEN);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_orphan_mutex_owner(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 50U, 2U, 1U);
    const lswg_node_ref_t mutex = ref(LSWG_NODE_MUTEX, 5U, 8U, 0U);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(mutex, 0U, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, mutex,
                                     LSWG_EDGE_TASK_WAITS_MUTEX,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_PROVEN_ORPHAN);
    TEST_CHECK(result.member_count == 1U);
    TEST_CHECK(result.members[0].kind == LSWG_NODE_MUTEX);
    TEST_CHECK(result.members[0].identity.primary == 5U);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_ready_channel_lost_wake(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 60U, 4U, 1U);
    const lswg_node_ref_t channel = ref(LSWG_NODE_CHANNEL, 9U, 3U, 0U);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph,
                        node(channel,
                             LSWG_NODE_READY_NOW |
                                 LSWG_NODE_MATCHABLE_STUCK,
                             1U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, channel,
                                     LSWG_EDGE_TASK_WAITS_CHANNEL_RECV,
                                     LSWG_EDGE_READY_NOW |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_MATCHABLE_LOST_WAKE);
    TEST_CHECK(result.members[0].kind == LSWG_NODE_CHANNEL);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_expired_timer(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 70U, 12U, 1U);
    const lswg_node_ref_t timer = ref(LSWG_NODE_TIMER, 70U, 12U, 100U);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph,
                        node(timer, LSWG_NODE_OVERDUE_STUCK, 100U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, timer,
                                     LSWG_EDGE_TASK_WAITS_TIMER,
                                     LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_OVERDUE_SOURCE);
    TEST_CHECK(result.members[0].kind == LSWG_NODE_TIMER);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_three_task_and_mixed_cycles(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task_1 = ref(LSWG_NODE_TASK, 101U, 1U, 1U);
    const lswg_node_ref_t task_2 = ref(LSWG_NODE_TASK, 102U, 1U, 1U);
    const lswg_node_ref_t task_3 = ref(LSWG_NODE_TASK, 103U, 1U, 1U);
    const lswg_node_ref_t mutex = ref(LSWG_NODE_MUTEX, 22U, 6U, 0U);

    TEST_CHECK(lswg_graph_init(&graph, 3U, 3U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task_1, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(task_2, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(task_3, 0U, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task_1, task_2,
                                     LSWG_EDGE_TASK_WAITS_JOIN,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(add_edge(&graph, edge(task_2, task_3,
                                     LSWG_EDGE_TASK_WAITS_JOIN,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(add_edge(&graph, edge(task_3, task_1,
                                     LSWG_EDGE_TASK_WAITS_JOIN,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_PROVEN_CYCLE);
    TEST_CHECK(result.total_member_count == 3U);
    lswg_graph_destroy(&graph);

    TEST_CHECK(lswg_graph_init(&graph, 3U, 3U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task_1, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(task_2, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(mutex, 0U, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task_1, mutex,
                                     LSWG_EDGE_TASK_WAITS_MUTEX,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(add_edge(&graph, edge(mutex, task_2,
                                     LSWG_EDGE_MUTEX_OWNED_BY_TASK,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(add_edge(&graph, edge(task_2, task_1,
                                     LSWG_EDGE_TASK_WAITS_JOIN,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_PROVEN_CYCLE);
    TEST_CHECK(result.total_member_count == 3U);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
expect_direct_source_verdict(lswg_node_kind_t source_kind,
                             uint32_t source_flags,
                             lswg_edge_kind_t wait_kind,
                             lswg_verdict_t expected)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 201U, 3U, 1U);
    const lswg_node_ref_t source = ref(source_kind, 202U, 4U, 5U);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(source, source_flags, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, source, wait_kind,
                                     LSWG_EDGE_AND_REQUIRED |
                                         LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == expected);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_wait_kind_seed_and_terminal_semantics(void)
{
    TEST_CHECK(expect_direct_source_verdict(
        LSWG_NODE_IO_REQ, LSWG_NODE_LIVE_BACKEND,
        LSWG_EDGE_TASK_WAITS_IO, LSWG_VERDICT_OPEN));
    TEST_CHECK(expect_direct_source_verdict(
        LSWG_NODE_BLOCK_JOB, LSWG_NODE_RUNNING_JOB,
        LSWG_EDGE_TASK_WAITS_BLOCK_JOB, LSWG_VERDICT_OPEN));
    TEST_CHECK(expect_direct_source_verdict(
        LSWG_NODE_CANCEL_TOKEN, LSWG_NODE_UNCANCELLED_TOKEN,
        LSWG_EDGE_TASK_CAN_CANCEL, LSWG_VERDICT_OPEN));
    TEST_CHECK(expect_direct_source_verdict(
        LSWG_NODE_CHANNEL, LSWG_NODE_READY_NOW,
        LSWG_EDGE_TASK_WAITS_CHANNEL_SEND, LSWG_VERDICT_OPEN));
    TEST_CHECK(expect_direct_source_verdict(
        LSWG_NODE_BLOCK_JOB,
        LSWG_NODE_TERMINAL_PROGRESS | LSWG_NODE_MATCHABLE_STUCK,
        LSWG_EDGE_TASK_WAITS_BLOCK_JOB,
        LSWG_VERDICT_MATCHABLE_LOST_WAKE));
    TEST_CHECK(expect_direct_source_verdict(
        LSWG_NODE_CANCEL_TOKEN, LSWG_NODE_MATCHABLE_STUCK,
        LSWG_EDGE_TASK_CAN_CANCEL, LSWG_VERDICT_MATCHABLE_LOST_WAKE));
    TEST_CHECK(expect_direct_source_verdict(
        LSWG_NODE_TASK,
        LSWG_NODE_TERMINAL_PROGRESS | LSWG_NODE_MATCHABLE_STUCK,
        LSWG_EDGE_TASK_WAITS_JOIN, LSWG_VERDICT_MATCHABLE_LOST_WAKE));
    TEST_CHECK(expect_direct_source_verdict(
        LSWG_NODE_IO_REQ, 0U, LSWG_EDGE_TASK_WAITS_IO,
        LSWG_VERDICT_PROVEN_ORPHAN));
    return true;
}

static bool
test_channel_host_wake_and_incomplete_dominance(void)
{
    lswg_graph_t graph;
    lswg_result_t result;
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 301U, 1U, 1U);
    const lswg_node_ref_t channel = ref(LSWG_NODE_CHANNEL, 31U, 2U, 0U);
    const lswg_node_ref_t host =
        ref(LSWG_NODE_EXTERNAL_SOURCE, 7U, 1U, 0U);

    TEST_CHECK(lswg_graph_init(&graph, 3U, 2U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(channel, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph,
                        node(host, LSWG_NODE_EXTERNAL_OPEN, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, channel,
                                     LSWG_EDGE_TASK_WAITS_CHANNEL_RECV,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(add_edge(&graph, edge(channel, host,
                                     LSWG_EDGE_RESOURCE_EXTERNAL_SIGNAL,
                                     LSWG_EDGE_EXTERNAL_OPEN)));
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_OPEN);
    lswg_graph_destroy(&graph);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 2U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(channel, 0U, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, channel,
                                     LSWG_EDGE_TASK_WAITS_CHANNEL_RECV,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(add_edge(&graph, edge(channel, task,
                                     LSWG_EDGE_CHANNEL_MATCHED_BY_TASK,
                                     LSWG_EDGE_AND_REQUIRED)));
    lswg_graph_mark_incomplete(&graph, LSWG_INCOMPLETE_LOCK_BUSY);
    TEST_CHECK(solve_graph(&graph, &result));
    TEST_CHECK(result.verdict == LSWG_VERDICT_INCOMPLETE);
    TEST_CHECK(result.incomplete_reasons == LSWG_INCOMPLETE_LOCK_BUSY);
    lswg_graph_destroy(&graph);
    return true;
}

#ifndef LSWG_NO_CONFIRMATION
typedef struct fingerprint_variant {
    uint64_t task_wait_generation;
    uint64_t mutex_generation;
    uint64_t io_generation;
    uint64_t select_completion;
    uint64_t mutex_owner_task;
    bool include_extra_member;
    uintptr_t raw_bias;
    uint64_t capture_seq;
} fingerprint_variant_t;

static bool
build_fingerprint_graph(lswg_graph_t *graph,
                        const fingerprint_variant_t *variant)
{
    const lswg_node_ref_t waiter =
        ref(LSWG_NODE_TASK, 401U, variant->task_wait_generation, 2U);
    const lswg_node_ref_t owner_1 = ref(LSWG_NODE_TASK, 402U, 3U, 2U);
    const lswg_node_ref_t owner_2 = ref(LSWG_NODE_TASK, 403U, 3U, 2U);
    const lswg_node_ref_t mutex =
        ref(LSWG_NODE_MUTEX, 51U, variant->mutex_generation, 0U);
    const lswg_node_ref_t select = ref(LSWG_NODE_SELECT, 401U, 8U, 0U);
    const lswg_node_ref_t channel = ref(LSWG_NODE_CHANNEL, 52U, 4U, 0U);
    const lswg_node_ref_t io =
        ref(LSWG_NODE_IO_REQ, 0x5000U, variant->io_generation, 9U);
    const lswg_node_ref_t extra =
        ref(LSWG_NODE_EXTERNAL_SOURCE, 99U, 1U, 0U);
    const lswg_node_ref_t owner =
        variant->mutex_owner_task == 402U ? owner_1 : owner_2;
    const size_t node_count = variant->include_extra_member ? 8U : 7U;

    TEST_CHECK(lswg_graph_init(graph, node_count, 3U, NULL) ==
               LSWG_STATUS_OK);
    graph->capture_seq = variant->capture_seq;
    TEST_CHECK(add_node(graph,
                        node(channel, 0U, 0U,
                             variant->raw_bias + 0x600U)));
    TEST_CHECK(add_node(graph,
                        node(owner_2, LSWG_NODE_RUNNABLE, 0U,
                             variant->raw_bias + 0x300U)));
    TEST_CHECK(add_node(graph,
                        node(waiter, 0U, 0U,
                             variant->raw_bias + 0x100U)));
    TEST_CHECK(add_node(graph,
                        node(io, 0U, 0U,
                             variant->raw_bias + 0x700U)));
    TEST_CHECK(add_node(graph,
                        node(mutex, 0U, 0U,
                             variant->raw_bias + 0x400U)));
    TEST_CHECK(add_node(graph,
                        node(owner_1, LSWG_NODE_RUNNABLE, 0U,
                             variant->raw_bias + 0x200U)));
    TEST_CHECK(add_node(graph,
                        node(select, 0U, variant->select_completion,
                             variant->raw_bias + 0x500U)));
    if (variant->include_extra_member) {
        TEST_CHECK(add_node(graph,
                            node(extra, LSWG_NODE_EXTERNAL_OPEN, 0U,
                                 variant->raw_bias + 0x800U)));
    }
    TEST_CHECK(add_edge(graph, edge(select, channel,
                                    LSWG_EDGE_SELECT_ALTERNATIVE,
                                    LSWG_EDGE_OR_ALTERNATIVE |
                                        LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(add_edge(graph, edge(waiter, mutex,
                                    LSWG_EDGE_TASK_WAITS_MUTEX,
                                    LSWG_EDGE_AND_REQUIRED |
                                        LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(add_edge(graph, edge(mutex, owner,
                                    LSWG_EDGE_MUTEX_OWNED_BY_TASK,
                                    LSWG_EDGE_AND_REQUIRED |
                                        LSWG_EDGE_GENERATION_STABLE)));
    TEST_CHECK(lswg_graph_finalize(graph) == LSWG_STATUS_OK);
    return true;
}

static bool
test_fingerprint_covers_semantics_not_addresses(void)
{
    static const fingerprint_variant_t baseline = {
        11U, 12U, 13U, 14U, 402U, false, 0x1000U, 1U};
    lswg_graph_t first;
    lswg_graph_t second;
    fingerprint_variant_t variant;
    uint64_t first_hash;
    uint64_t second_hash;

    TEST_CHECK(build_fingerprint_graph(&first, &baseline));
    variant = baseline;
    variant.raw_bias = 0x900000U;
    variant.capture_seq = 999U;
    TEST_CHECK(build_fingerprint_graph(&second, &variant));
    TEST_CHECK(lswg_fingerprint(&first, &first_hash) == LSWG_STATUS_OK);
    TEST_CHECK(lswg_fingerprint(&second, &second_hash) == LSWG_STATUS_OK);
    TEST_CHECK(first_hash == second_hash);
    lswg_graph_destroy(&second);

#define EXPECT_FINGERPRINT_CHANGE(field, value)                                 \
    do {                                                                        \
        variant = baseline;                                                     \
        variant.field = (value);                                                \
        TEST_CHECK(build_fingerprint_graph(&second, &variant));                 \
        TEST_CHECK(lswg_fingerprint(&second, &second_hash) ==                  \
                   LSWG_STATUS_OK);                                             \
        TEST_CHECK(first_hash != second_hash);                                  \
        lswg_graph_destroy(&second);                                            \
    } while (0)

    EXPECT_FINGERPRINT_CHANGE(task_wait_generation, 21U);
    EXPECT_FINGERPRINT_CHANGE(mutex_generation, 22U);
    EXPECT_FINGERPRINT_CHANGE(io_generation, 23U);
    EXPECT_FINGERPRINT_CHANGE(select_completion, 24U);
    EXPECT_FINGERPRINT_CHANGE(mutex_owner_task, 403U);
    EXPECT_FINGERPRINT_CHANGE(include_extra_member, true);
#undef EXPECT_FINGERPRINT_CHANGE

    lswg_graph_destroy(&first);
    return true;
}

static bool
build_generation_snapshot(lswg_graph_t *graph, uint64_t wait_generation)
{
    const lswg_node_ref_t task =
        ref(LSWG_NODE_TASK, 80U, wait_generation, 1U);
    const lswg_node_ref_t mutex = ref(LSWG_NODE_MUTEX, 12U, 3U, 0U);

    TEST_CHECK(lswg_graph_init(graph, 2U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(graph, node(task, 0U, 0U, 0xabcdefU)));
    TEST_CHECK(add_node(graph, node(mutex, 0U, 0U, 0x123456U)));
    TEST_CHECK(add_edge(graph, edge(task, mutex,
                                    LSWG_EDGE_TASK_WAITS_MUTEX,
                                    LSWG_EDGE_GENERATION_STABLE)));
    return true;
}

static bool
test_unstable_generation_between_snapshots(void)
{
    lswg_graph_t first_graph;
    lswg_graph_t second_graph;
    lswg_result_t first;
    lswg_result_t second;
    lswg_result_t confirmed;

    TEST_CHECK(build_generation_snapshot(&first_graph, 4U));
    TEST_CHECK(build_generation_snapshot(&second_graph, 5U));
    TEST_CHECK(solve_graph(&first_graph, &first));
    TEST_CHECK(solve_graph(&second_graph, &second));
    TEST_CHECK(first.verdict == LSWG_VERDICT_PROVEN_ORPHAN);
    TEST_CHECK(second.verdict == LSWG_VERDICT_PROVEN_ORPHAN);
    TEST_CHECK(lswg_confirm(&first, 44U, &second, 44U, &confirmed) ==
               LSWG_STATUS_OK);
    TEST_CHECK(confirmed.verdict == LSWG_VERDICT_PROGRESS_CHANGED);
    TEST_CHECK(!confirmed.confirmed);
    lswg_graph_destroy(&first_graph);
    lswg_graph_destroy(&second_graph);
    return true;
}

static bool
test_confirmation_gates_progress_open_and_incomplete(void)
{
    lswg_graph_t first_graph;
    lswg_graph_t second_graph;
    lswg_result_t first;
    lswg_result_t second;
    lswg_result_t confirmed;
    fingerprint_variant_t variant = {
        11U, 12U, 13U, 14U, 402U, false, 0x1000U, 1U};

    TEST_CHECK(build_fingerprint_graph(&first_graph, &variant));
    variant.raw_bias = 0x500000U;
    variant.capture_seq = 2U;
    TEST_CHECK(build_fingerprint_graph(&second_graph, &variant));
    {
        lswg_workspace_t workspace;

        TEST_CHECK(lswg_workspace_init(&workspace, first_graph.node_count,
                                       first_graph.edge_count, NULL) ==
                   LSWG_STATUS_OK);
        TEST_CHECK(lswg_solve(&first_graph, &workspace, &first) ==
                   LSWG_STATUS_OK);
        TEST_CHECK(lswg_solve(&second_graph, &workspace, &second) ==
                   LSWG_STATUS_OK);
        lswg_workspace_destroy(&workspace);
    }
    TEST_CHECK(first.verdict == LSWG_VERDICT_OPEN);
    TEST_CHECK(second.verdict == LSWG_VERDICT_OPEN);
    TEST_CHECK(lswg_confirm(&first, 70U, &second, 70U, &confirmed) ==
               LSWG_STATUS_OK);
    TEST_CHECK(confirmed.verdict == LSWG_VERDICT_OPEN);
    TEST_CHECK(!confirmed.confirmed);
    TEST_CHECK(lswg_confirm(&first, 70U, &second, 71U, &confirmed) ==
               LSWG_STATUS_OK);
    TEST_CHECK(confirmed.verdict == LSWG_VERDICT_PROGRESS_CHANGED);
    TEST_CHECK(!confirmed.confirmed);
    lswg_graph_destroy(&first_graph);
    lswg_graph_destroy(&second_graph);

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.verdict = LSWG_VERDICT_INCOMPLETE;
    second.verdict = LSWG_VERDICT_INCOMPLETE;
    first.incomplete_reasons = LSWG_INCOMPLETE_LOCK_BUSY;
    second.incomplete_reasons = LSWG_INCOMPLETE_ALLOCATION;
    TEST_CHECK(lswg_confirm(&first, 8U, &second, 8U, &confirmed) ==
               LSWG_STATUS_OK);
    TEST_CHECK(confirmed.verdict == LSWG_VERDICT_INCOMPLETE);
    TEST_CHECK(confirmed.incomplete_reasons ==
               (LSWG_INCOMPLETE_LOCK_BUSY | LSWG_INCOMPLETE_ALLOCATION));
    TEST_CHECK(!confirmed.confirmed);
    return true;
}

static bool
test_stable_orphan_is_confirmed(void)
{
    lswg_graph_t first_graph;
    lswg_graph_t second_graph;
    lswg_result_t first;
    lswg_result_t second;
    lswg_result_t confirmed;

    TEST_CHECK(build_generation_snapshot(&first_graph, 19U));
    TEST_CHECK(build_generation_snapshot(&second_graph, 19U));
    first_graph.capture_seq = 100U;
    second_graph.capture_seq = 101U;
    first_graph.nodes[0].desc.raw_address += 0x100000U;
    second_graph.nodes[0].desc.raw_address += 0x900000U;
    TEST_CHECK(solve_graph(&first_graph, &first));
    TEST_CHECK(solve_graph(&second_graph, &second));
    TEST_CHECK(first.fingerprint == second.fingerprint);
    TEST_CHECK(lswg_confirm(&first, 90U, &second, 90U, &confirmed) ==
               LSWG_STATUS_OK);
    TEST_CHECK(confirmed.verdict == LSWG_VERDICT_PROVEN_ORPHAN);
    TEST_CHECK(confirmed.confirmed);
    lswg_graph_destroy(&first_graph);
    lswg_graph_destroy(&second_graph);
    return true;
}
#endif
#endif

static bool
test_generation_identity_and_raw_address_rules(void)
{
    lswg_graph_t graph;
    lswg_node_desc_t first =
        node(ref(LSWG_NODE_MUTEX, 7U, 1U, 0U), 0U, 0U, 0x1000U);
    lswg_node_desc_t reused =
        node(ref(LSWG_NODE_MUTEX, 7U, 2U, 0U), 0U, 0U, 0x1000U);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 0U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, first));
    TEST_CHECK(add_node(&graph, reused));
    TEST_CHECK(lswg_graph_finalize(&graph) == LSWG_STATUS_OK);
    lswg_graph_destroy(&graph);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 0U, NULL) == LSWG_STATUS_OK);
    first.raw_address = 0x2000U;
    reused = first;
    reused.raw_address = 0x3000U;
    TEST_CHECK(add_node(&graph, first));
    TEST_CHECK(add_node(&graph, reused));
    TEST_CHECK(lswg_graph_finalize(&graph) ==
               LSWG_STATUS_DUPLICATE_IDENTITY);
    TEST_CHECK(!graph.finalized);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_graph_rejects_malformed_input_transactionally(void)
{
    lswg_graph_t graph;
    lswg_node_desc_t invalid =
        node(ref(LSWG_NODE_KIND_COUNT, 1U, 1U, 1U), 0U, 0U, 0U);
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 1U, 1U, 1U);
    const lswg_node_ref_t missing = ref(LSWG_NODE_MUTEX, 1U, 1U, 0U);
    lswg_edge_desc_t missing_edge =
        edge(task, missing, LSWG_EDGE_TASK_WAITS_MUTEX,
             LSWG_EDGE_AND_REQUIRED);

    TEST_CHECK(lswg_graph_init(&graph, 1U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(lswg_graph_add_node(&graph, &invalid) ==
               LSWG_STATUS_INVALID_KIND);
    TEST_CHECK(graph.node_count == 0U);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(lswg_graph_add_node(&graph, &invalid) ==
               LSWG_STATUS_INVALID_KIND);
    TEST_CHECK(graph.node_count == 1U);
    TEST_CHECK(add_edge(&graph, missing_edge));
    TEST_CHECK(lswg_graph_finalize(&graph) == LSWG_STATUS_MISSING_ENDPOINT);
    TEST_CHECK(!graph.finalized);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_malformed_select_is_rejected(void)
{
    lswg_graph_t graph;
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 3U, 1U, 1U);
    const lswg_node_ref_t select = ref(LSWG_NODE_SELECT, 3U, 8U, 0U);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(select, 0U, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, select,
                                     LSWG_EDGE_TASK_WAITS_SELECT,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(lswg_graph_finalize(&graph) == LSWG_STATUS_MALFORMED_SELECT);
    TEST_CHECK(!graph.finalized);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_finalized_order_is_deterministic(void)
{
    lswg_graph_t first;
    lswg_graph_t second;
    const lswg_node_ref_t task_1 = ref(LSWG_NODE_TASK, 1U, 2U, 1U);
    const lswg_node_ref_t task_2 = ref(LSWG_NODE_TASK, 2U, 2U, 1U);
    const lswg_node_ref_t mutex = ref(LSWG_NODE_MUTEX, 5U, 4U, 0U);
    uint64_t first_fingerprint;
    uint64_t second_fingerprint;
    size_t index;

    TEST_CHECK(lswg_graph_init(&first, 3U, 3U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(lswg_graph_init(&second, 3U, 3U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&first, node(mutex, 0U, 3U, 0x9000U)));
    TEST_CHECK(add_node(&first, node(task_2, 0U, 2U, 0x8000U)));
    TEST_CHECK(add_node(&first, node(task_1, 0U, 1U, 0x7000U)));
    TEST_CHECK(add_node(&second, node(task_1, 0U, 1U, 0x17000U)));
    TEST_CHECK(add_node(&second, node(mutex, 0U, 3U, 0x19000U)));
    TEST_CHECK(add_node(&second, node(task_2, 0U, 2U, 0x18000U)));

    TEST_CHECK(add_edge(&first, edge(mutex, task_1,
                                     LSWG_EDGE_MUTEX_OWNED_BY_TASK, 1U)));
    TEST_CHECK(add_edge(&first, edge(task_2, mutex,
                                     LSWG_EDGE_TASK_WAITS_MUTEX, 2U)));
    TEST_CHECK(add_edge(&first, edge(task_1, task_2,
                                     LSWG_EDGE_TASK_WAITS_JOIN, 3U)));
    TEST_CHECK(add_edge(&second, edge(task_1, task_2,
                                      LSWG_EDGE_TASK_WAITS_JOIN, 3U)));
    TEST_CHECK(add_edge(&second, edge(mutex, task_1,
                                      LSWG_EDGE_MUTEX_OWNED_BY_TASK, 1U)));
    TEST_CHECK(add_edge(&second, edge(task_2, mutex,
                                      LSWG_EDGE_TASK_WAITS_MUTEX, 2U)));
    TEST_CHECK(lswg_graph_finalize(&first) == LSWG_STATUS_OK);
    TEST_CHECK(lswg_graph_finalize(&second) == LSWG_STATUS_OK);
    for (index = 0U; index < first.node_count; ++index) {
        TEST_CHECK(first.nodes[index].desc.ref.kind ==
                   second.nodes[index].desc.ref.kind);
        TEST_CHECK(first.nodes[index].desc.ref.identity.primary ==
                   second.nodes[index].desc.ref.identity.primary);
    }
    for (index = 0U; index < first.edge_count; ++index) {
        TEST_CHECK(first.edges[index].source_index ==
                   second.edges[index].source_index);
        TEST_CHECK(first.edges[index].target_index ==
                   second.edges[index].target_index);
        TEST_CHECK(first.edges[index].desc.kind ==
                   second.edges[index].desc.kind);
    }
    TEST_CHECK(lswg_fingerprint(&first, &first_fingerprint) ==
               LSWG_STATUS_OK);
    TEST_CHECK(lswg_fingerprint(&second, &second_fingerprint) ==
               LSWG_STATUS_OK);
    TEST_CHECK(first_fingerprint == second_fingerprint);
    lswg_graph_destroy(&first);
    lswg_graph_destroy(&second);
    return true;
}

typedef struct allocation_probe {
    size_t attempts;
    size_t live_blocks;
    size_t fail_at_attempt;
} allocation_probe_t;

static void *
probe_allocate(void *context, size_t size)
{
    allocation_probe_t *probe = context;
    void *allocation;

    probe->attempts += 1U;
    if (probe->fail_at_attempt != 0U &&
        probe->attempts >= probe->fail_at_attempt) {
        return NULL;
    }
    allocation = malloc(size);
    if (allocation != NULL) {
        probe->live_blocks += 1U;
    }
    return allocation;
}

static void
probe_deallocate(void *context, void *pointer)
{
    allocation_probe_t *probe = context;

    if (pointer != NULL) {
        if (probe->live_blocks == 0U) {
            abort();
        }
        probe->live_blocks -= 1U;
    }
    free(pointer);
}

static bool
test_workspace_and_growth_failures_are_inconclusive(void)
{
    allocation_probe_t graph_probe = {0U, 0U, 2U};
    allocation_probe_t workspace_probe = {0U, 0U, 4U};
    lswg_allocator_t graph_allocator = {
        &graph_probe, probe_allocate, probe_deallocate};
    lswg_allocator_t workspace_allocator = {
        &workspace_probe, probe_allocate, probe_deallocate};
    lswg_graph_t graph;
    lswg_workspace_t workspace;
    lswg_result_t result;
    const lswg_node_ref_t task = ref(LSWG_NODE_TASK, 501U, 1U, 1U);
    const lswg_node_ref_t mutex = ref(LSWG_NODE_MUTEX, 61U, 1U, 0U);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 1U, &graph_allocator) ==
               LSWG_STATUS_OUT_OF_MEMORY);
    TEST_CHECK(graph_probe.live_blocks == 0U);
    TEST_CHECK(lswg_workspace_init(&workspace, 4U, 4U,
                                   &workspace_allocator) ==
               LSWG_STATUS_OUT_OF_MEMORY);
    TEST_CHECK(workspace_probe.live_blocks == 0U);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(mutex, 0U, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, mutex,
                                     LSWG_EDGE_TASK_WAITS_MUTEX,
                                     LSWG_EDGE_AND_REQUIRED)));
    TEST_CHECK(lswg_graph_finalize(&graph) == LSWG_STATUS_OK);
    TEST_CHECK(lswg_workspace_init(&workspace, 1U, 1U, NULL) ==
               LSWG_STATUS_OK);
    TEST_CHECK(lswg_solve(&graph, &workspace, &result) == LSWG_STATUS_OK);
    TEST_CHECK(result.verdict == LSWG_VERDICT_INCOMPLETE);
    TEST_CHECK((result.incomplete_reasons & LSWG_INCOMPLETE_WORKSPACE) != 0U);
    lswg_workspace_destroy(&workspace);
    lswg_graph_destroy(&graph);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(add_node(&graph, node(mutex, 0U, 0U, 0U)));
    TEST_CHECK(add_edge(&graph, edge(task, mutex,
                                     LSWG_EDGE_TASK_WAITS_MUTEX,
                                     LSWG_EDGE_AND_REQUIRED)));
    lswg_graph_mark_incomplete(&graph, LSWG_INCOMPLETE_ALLOCATION);
    TEST_CHECK(lswg_graph_finalize(&graph) == LSWG_STATUS_OK);
    TEST_CHECK(lswg_workspace_init(&workspace, 2U, 1U, NULL) ==
               LSWG_STATUS_OK);
    TEST_CHECK(lswg_solve(&graph, &workspace, &result) == LSWG_STATUS_OK);
    TEST_CHECK(result.verdict == LSWG_VERDICT_INCOMPLETE);
    TEST_CHECK((result.incomplete_reasons & LSWG_INCOMPLETE_ALLOCATION) !=
               0U);
    lswg_workspace_destroy(&workspace);
    lswg_graph_destroy(&graph);

    TEST_CHECK(lswg_graph_init(&graph, 2U, 1U, NULL) == LSWG_STATUS_OK);
    TEST_CHECK(add_node(&graph, node(task, 0U, 0U, 0U)));
    TEST_CHECK(lswg_synthetic_populate(
                   &graph, 100U, LSWG_SYNTHETIC_LONG_CHAIN_OPEN) ==
               LSWG_STATUS_CAPACITY);
    TEST_CHECK(graph.node_count == 1U);
    TEST_CHECK(graph.nodes[0].desc.ref.identity.primary == 501U);
    lswg_graph_destroy(&graph);
    return true;
}

static bool
test_scale_profiles_are_bounded_and_deterministic(void)
{
    static const lswg_verdict_t expected[] = {
        LSWG_VERDICT_OPEN,
        LSWG_VERDICT_PROVEN_CYCLE,
        LSWG_VERDICT_OPEN,
        LSWG_VERDICT_PROVEN_ORPHAN,
    };
    allocation_probe_t probe = {0U, 0U, 0U};
    lswg_allocator_t allocator = {&probe, probe_allocate, probe_deallocate};
    lswg_graph_t graph;
    lswg_workspace_t workspace;
    size_t allocations_after_init;
    clock_t solve_ticks = 0;
    size_t profile;

    TEST_CHECK(lswg_graph_init(&graph, scale_node_count, scale_node_count,
                               &allocator) == LSWG_STATUS_OK);
    TEST_CHECK(lswg_workspace_init(&workspace, scale_node_count,
                                   scale_node_count, &allocator) ==
               LSWG_STATUS_OK);
    allocations_after_init = probe.attempts;

    for (profile = 0U; profile < sizeof(expected) / sizeof(expected[0]);
         ++profile) {
        lswg_result_t baseline;
        size_t repetition;

        TEST_CHECK(lswg_synthetic_populate(
                       &graph, scale_node_count,
                       (lswg_synthetic_profile_t)profile) == LSWG_STATUS_OK);
        TEST_CHECK(lswg_graph_finalize(&graph) == LSWG_STATUS_OK);
        TEST_CHECK(lswg_solve(&graph, &workspace, &baseline) ==
                   LSWG_STATUS_OK);
        TEST_CHECK(baseline.verdict == expected[profile]);
        for (repetition = 0U; repetition < 5U; ++repetition) {
            lswg_result_t result;
            const clock_t begin = clock();

            TEST_CHECK(lswg_solve(&graph, &workspace, &result) ==
                       LSWG_STATUS_OK);
            solve_ticks += clock() - begin;
            TEST_CHECK(result.verdict == baseline.verdict);
            TEST_CHECK(result.fingerprint == baseline.fingerprint);
            TEST_CHECK(result.member_count == baseline.member_count);
            if (result.member_count != 0U) {
                TEST_CHECK(result.members[0].kind == baseline.members[0].kind);
                TEST_CHECK(result.members[0].identity.primary ==
                           baseline.members[0].identity.primary);
            }
        }
        TEST_CHECK(probe.attempts == allocations_after_init);
    }

    if (emit_scale_timing) {
        const double elapsed_ms =
            ((double)solve_ticks * 1000.0) / (double)CLOCKS_PER_SEC;
        const double budget_ms = 5000.0;

        printf("LSWG_SCALE nodes=%zu profiles=4 solves=20 "
               "elapsed_ms=%.3f budget_ms=%.0f status=%s\n",
               scale_node_count, elapsed_ms, budget_ms,
               elapsed_ms <= budget_ms ? "PASS" : "FAIL");
        TEST_CHECK(elapsed_ms <= budget_ms);
    }

    lswg_workspace_destroy(&workspace);
    lswg_graph_destroy(&graph);
    TEST_CHECK(probe.live_blocks == 0U);
    return true;
}

typedef bool (*test_fn)(void);

typedef struct test_case {
    const char *name;
    test_fn function;
} test_case_t;

int
main(int argc, char **argv)
{
    static const test_case_t tests[] = {
#ifndef LSWG_GRAPH_ONLY
        {"two_task_mutex_cycle", test_two_task_mutex_cycle},
        {"join_cycle", test_join_cycle},
        {"open_world_condition", test_open_world_condition},
        {"select_future_timer_alternative",
         test_select_future_timer_alternative},
        {"owner_blocked_on_live_io", test_owner_blocked_on_live_io},
        {"orphan_mutex_owner", test_orphan_mutex_owner},
        {"ready_channel_lost_wake", test_ready_channel_lost_wake},
        {"expired_timer", test_expired_timer},
        {"three_task_and_mixed_cycles", test_three_task_and_mixed_cycles},
        {"wait_kind_seed_and_terminal_semantics",
         test_wait_kind_seed_and_terminal_semantics},
        {"channel_host_wake_and_incomplete_dominance",
         test_channel_host_wake_and_incomplete_dominance},
#ifndef LSWG_NO_CONFIRMATION
        {"fingerprint_covers_semantics_not_addresses",
         test_fingerprint_covers_semantics_not_addresses},
        {"unstable_generation_between_snapshots",
         test_unstable_generation_between_snapshots},
        {"confirmation_gates_progress_open_and_incomplete",
         test_confirmation_gates_progress_open_and_incomplete},
        {"stable_orphan_is_confirmed", test_stable_orphan_is_confirmed},
#endif
#endif
        {"generation_identity_and_raw_address_rules",
         test_generation_identity_and_raw_address_rules},
        {"graph_rejects_malformed_input_transactionally",
         test_graph_rejects_malformed_input_transactionally},
        {"malformed_select_is_rejected", test_malformed_select_is_rejected},
        {"finalized_order_is_deterministic",
         test_finalized_order_is_deterministic},
        {"workspace_and_growth_failures_are_inconclusive",
         test_workspace_and_growth_failures_are_inconclusive},
        {"scale_profiles_are_bounded_and_deterministic",
         test_scale_profiles_are_bounded_and_deterministic},
    };
    size_t index;

    if (argc == 2 && strcmp(argv[1], "--scale") == 0) {
        scale_node_count = 100000U;
        emit_scale_timing = true;
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--scale]\n", argv[0]);
        return 2;
    }

    for (index = 0U; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (!tests[index].function()) {
            fprintf(stderr, "FAIL %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }

    printf("LSWG phase0 contract tests passed (%zu cases)\n",
           sizeof(tests) / sizeof(tests[0]));
    return 0;
}
