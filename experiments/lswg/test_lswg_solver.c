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

typedef bool (*test_fn)(void);

typedef struct test_case {
    const char *name;
    test_fn function;
} test_case_t;

int
main(void)
{
    static const test_case_t tests[] = {
        {"two_task_mutex_cycle", test_two_task_mutex_cycle},
        {"join_cycle", test_join_cycle},
        {"open_world_condition", test_open_world_condition},
        {"select_future_timer_alternative",
         test_select_future_timer_alternative},
        {"owner_blocked_on_live_io", test_owner_blocked_on_live_io},
        {"orphan_mutex_owner", test_orphan_mutex_owner},
        {"ready_channel_lost_wake", test_ready_channel_lost_wake},
        {"expired_timer", test_expired_timer},
        {"unstable_generation_between_snapshots",
         test_unstable_generation_between_snapshots},
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
