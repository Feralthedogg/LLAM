/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lswg_graph.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TEST_CHECK(condition)                                                   \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return false;                                                       \
        }                                                                       \
    } while (0)

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
    lswg_graph_destroy(&first);
    lswg_graph_destroy(&second);
    return true;
}

typedef bool (*test_fn)(void);

typedef struct test_case {
    const char *name;
    test_fn function;
} test_case_t;

int
main(void)
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
        {"unstable_generation_between_snapshots",
         test_unstable_generation_between_snapshots},
#endif
#endif
        {"generation_identity_and_raw_address_rules",
         test_generation_identity_and_raw_address_rules},
        {"graph_rejects_malformed_input_transactionally",
         test_graph_rejects_malformed_input_transactionally},
        {"malformed_select_is_rejected", test_malformed_select_is_rejected},
        {"finalized_order_is_deterministic",
         test_finalized_order_is_deterministic},
    };
    size_t index;

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
