/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lccf_fact.h"
#include "lccf_platform.h"

#include <errno.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "[test_lccf_fact] %s:%d: %s\n",                  \
                    __FILE__, __LINE__, (message));                            \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int record_site_invocation(
    const lccf_fact_site_descriptor_t *descriptor,
    const lccf_fact_core_t *fact,
    void *context) {
    unsigned *calls = context;

    if (descriptor == NULL || fact == NULL || calls == NULL ||
        descriptor != fact->resolved_site ||
        descriptor->logical_index != fact->site_index) {
        return EPROTO;
    }
    *calls += 1U;
    return 0;
}

static const lccf_fact_site_descriptor_t TEST_SITE_DESCRIPTORS[8] = {
    {record_site_invocation, 0U, 0U},
    {record_site_invocation, 1U, 0U},
    {record_site_invocation, 2U, 0U},
    {record_site_invocation, 3U, 0U},
    {record_site_invocation, 4U, 0U},
    {record_site_invocation, 5U, 0U},
    {record_site_invocation, 6U, 0U},
    {record_site_invocation, 7U, 0U},
};

static const lccf_fact_site_descriptor_t *const TEST_SITE_TABLE[8] = {
    &TEST_SITE_DESCRIPTORS[0],
    &TEST_SITE_DESCRIPTORS[1],
    &TEST_SITE_DESCRIPTORS[2],
    &TEST_SITE_DESCRIPTORS[3],
    &TEST_SITE_DESCRIPTORS[4],
    &TEST_SITE_DESCRIPTORS[5],
    &TEST_SITE_DESCRIPTORS[6],
    &TEST_SITE_DESCRIPTORS[7],
};

static lccf_fact_ticket_t ticket_for(lccf_fact_source_t source,
                                     uint64_t generation) {
    lccf_fact_ticket_t ticket;
    const int rc = lccf_fact_ticket_from_logical(
        source, LCCF_FACT_EVENT_IO, generation, 37, EAGAIN,
        UINT64_C(0xfeedface), 3U, 8U, 2U, 5U, 0U,
        TEST_SITE_TABLE, &ticket);

    if (rc != 0) {
        memset(&ticket, 0, sizeof(ticket));
        ticket.generation = UINT64_MAX;
    }
    return ticket;
}

static lccf_fact_guard_t direct_guard(uint32_t shard) {
    lccf_fact_guard_t guard;

    memset(&guard, 0, sizeof(guard));
    guard.flags = LCCF_FACT_GUARD_DIRECT_ENABLED |
                  LCCF_FACT_GUARD_MODULE_ENABLED |
                  LCCF_FACT_GUARD_BACKEND_CAPABLE;
    guard.budget_remaining = 1U;
    guard.current_home_shard = shard;
    guard.consuming_shard = shard;
    return guard;
}

static int test_layout_contracts(void) {
    CHECK(sizeof(lccf_fact_core_t) == 64U, "fact core is not 64 bytes");
    CHECK(sizeof(lccf_fact_split64_64_layout_t) == 128U,
          "split64_64 layout size");
    CHECK(sizeof(lccf_fact_split96_64_layout_t) == 160U,
          "split96_64 layout size");
    CHECK(sizeof(lccf_fact_unified128_layout_t) == 128U,
          "unified128 layout size");
    CHECK(offsetof(lccf_fact_split64_64_layout_t, sidecar) == 64U &&
              offsetof(lccf_fact_split96_64_layout_t, sidecar) == 96U &&
              offsetof(lccf_fact_unified128_layout_t, fact) == 64U,
          "fact storage offsets must match declared layouts");
    CHECK(alignof(lccf_fact_core_t) >= alignof(uint64_t),
          "fact core alignment");
    CHECK(alignof(lccf_fact_cell_t) >= alignof(uint64_t),
          "fact cell alignment");
    CHECK(lccf_fact_layout_hot_bytes(LCCF_FACT_LAYOUT_SPLIT64_64) == 64U &&
              lccf_fact_layout_sidecar_bytes(
                  LCCF_FACT_LAYOUT_SPLIT64_64) == 64U,
          "split64_64 footprint");
    CHECK(lccf_fact_layout_hot_bytes(LCCF_FACT_LAYOUT_SPLIT96_64) == 96U &&
              lccf_fact_layout_sidecar_bytes(
                  LCCF_FACT_LAYOUT_SPLIT96_64) == 64U,
          "split96_64 footprint");
    CHECK(lccf_fact_layout_hot_bytes(LCCF_FACT_LAYOUT_UNIFIED128) == 128U &&
              lccf_fact_layout_sidecar_bytes(
                  LCCF_FACT_LAYOUT_UNIFIED128) == 0U,
          "unified128 footprint");
    CHECK(lccf_fact_layout_hot_bytes((lccf_fact_layout_t)99) == 0U &&
              lccf_fact_layout_sidecar_bytes(
                  (lccf_fact_layout_t)99) == 0U,
          "unknown layout footprint");
    return 0;
}

static bool canonical_event_equal(const lccf_fact_core_t *left,
                                  const lccf_fact_core_t *right) {
    return left->generation == right->generation &&
           left->fact_id == right->fact_id &&
           left->stable_flags == right->stable_flags &&
           left->result == right->result &&
           left->payload_word == right->payload_word &&
           left->error_code == right->error_code &&
           left->event_kind == right->event_kind &&
           left->captured_home_shard == right->captured_home_shard &&
           left->source_node == right->source_node &&
           left->site_index == right->site_index;
}

static int test_platform_adapters_normalize_equivalent_events(void) {
    static const lccf_fact_source_t sources[] = {
        LCCF_FACT_SOURCE_LINUX_CQE,
        LCCF_FACT_SOURCE_KQUEUE,
        LCCF_FACT_SOURCE_IOCP,
    };
    lccf_fact_core_t facts[sizeof(sources) / sizeof(sources[0])];
    lccf_fact_core_t published[sizeof(sources) / sizeof(sources[0])];
    size_t index;

    for (index = 0U; index < sizeof(sources) / sizeof(sources[0]); ++index) {
        lccf_fact_ticket_t ticket = ticket_for(sources[index], 17U);

        CHECK(lccf_fact_normalize(&ticket, UINT64_C(0xabc),
                                  &facts[index]) == 0,
              "platform normalization");
        CHECK(facts[index].source_kind == (uint32_t)sources[index],
              "source history is preserved");
        if (index != 0U) {
            CHECK(canonical_event_equal(&facts[0], &facts[index]),
                  "platform adapters disagree on canonical event");
        }
        {
            lccf_fact_cell_t cell;
            lccf_fact_counters_t counters;
            bool won = false;

            memset(&counters, 0, sizeof(counters));
            CHECK(lccf_fact_cell_init(&cell, 17U,
                                      LCCF_FACT_LAYOUT_SPLIT64_64,
                                      1U, NULL) == 0,
                  "platform publication cell initialization");
            CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters,
                                        &won) == 0 && won,
                  "platform fact publication");
            CHECK(lccf_fact_acquire(&cell, 17U, &published[index]) == 0,
                  "platform fact acquisition");
            if (index != 0U) {
                CHECK(canonical_event_equal(&published[0],
                                            &published[index]),
                      "platform facts receive different semantic identity");
            }
        }
    }
    return 0;
}

static int test_shared_fact_removes_repeated_materialization_work(void) {
    lccf_fact_cell_t shared_cell;
    lccf_fact_cell_t recompute_cell;
    lccf_fact_counters_t shared_counters;
    lccf_fact_counters_t recompute_counters;
    lccf_fact_ticket_t ticket = ticket_for(LCCF_FACT_SOURCE_LINUX_CQE, 31U);
    lccf_fact_core_t shared_fact;
    lccf_fact_core_t recomputed_fact;
    lccf_fact_core_t published_fact;
    bool won = false;

    memset(&shared_counters, 0, sizeof(shared_counters));
    memset(&recompute_counters, 0, sizeof(recompute_counters));
    CHECK(lccf_fact_cell_init(&shared_cell, 31U,
                              LCCF_FACT_LAYOUT_SPLIT64_64, 1U, NULL) == 0,
          "shared cell initialization");
    CHECK(lccf_fact_cell_init(&recompute_cell, 31U,
                              LCCF_FACT_LAYOUT_SPLIT64_64, 1U, NULL) == 0,
          "recompute cell initialization");
    CHECK(lccf_fact_try_publish(&shared_cell, &ticket, true,
                                &shared_counters, &won) == 0 && won,
          "shared publication");
    won = false;
    CHECK(lccf_fact_try_publish(&recompute_cell, &ticket, false,
                                &recompute_counters, &won) == 0 && won,
          "recompute publication");
    CHECK(shared_counters.normalization_calls == 1U &&
              shared_counters.site_lookups == 1U,
          "shared publication performs work once");
    CHECK(recompute_counters.normalization_calls == 0U &&
              recompute_counters.site_lookups == 0U,
          "recompute publication defers work");

    CHECK(lccf_fact_materialize(&shared_cell, 31U, 3U, &shared_counters,
                                &shared_fact) == 0,
          "first shared materialization");
    CHECK(lccf_fact_materialize(&shared_cell, 31U, 3U, &shared_counters,
                                &shared_fact) == 0,
          "second shared materialization");
    CHECK(lccf_fact_materialize(&recompute_cell, 31U, 3U,
                                &recompute_counters,
                                &recomputed_fact) == 0,
          "first recompute materialization");
    CHECK(lccf_fact_materialize(&recompute_cell, 31U, 3U,
                                &recompute_counters,
                                &recomputed_fact) == 0,
          "second recompute materialization");
    CHECK(shared_counters.normalization_calls == 1U &&
              shared_counters.site_lookups == 1U,
          "shared consumers copy immutable fact");
    CHECK(recompute_counters.normalization_calls == 2U &&
              recompute_counters.site_lookups == 2U,
          "baseline consumers repeat materialization work");
    CHECK(canonical_event_equal(&shared_fact, &recomputed_fact),
          "optimization changes materialization work only");
    CHECK(lccf_fact_materialize(&shared_cell, 31U, 5U, &shared_counters,
                                &shared_fact) == 0 &&
              lccf_fact_materialize(&recompute_cell, 31U, 5U,
                                    &recompute_counters,
                                    &recomputed_fact) == 0,
          "continuation-site materialization");
    CHECK(shared_fact.resolved_site == TEST_SITE_TABLE[5] &&
              recomputed_fact.resolved_site == TEST_SITE_TABLE[5] &&
              canonical_event_equal(&shared_fact, &recomputed_fact),
          "continuation site resolves identically");
    CHECK(shared_counters.normalization_calls == 1U &&
              shared_counters.site_lookups == 2U &&
              recompute_counters.normalization_calls == 3U &&
              recompute_counters.site_lookups == 3U,
          "continuation dispatch does not renormalize shared event");
    CHECK(lccf_fact_acquire(&shared_cell, 31U, &published_fact) == 0 &&
              published_fact.site_index == 3U &&
              published_fact.resolved_site == TEST_SITE_TABLE[3],
          "continuation lookup cannot mutate published fact");
    return 0;
}

static int test_failure_paths_publish_or_retire_without_leaks(void) {
    lccf_fact_cell_t malformed_cell;
    lccf_fact_cell_t payload_cell;
    lccf_fact_cell_t module_cell;
    lccf_fact_counters_t counters;
    lccf_fact_ticket_t ticket;
    lccf_fact_core_t fact;
    bool won = false;
    size_t index;

    memset(&counters, 0, sizeof(counters));
    ticket = ticket_for(LCCF_FACT_SOURCE_LINUX_CQE, 41U);
    ticket.raw_flags |= LCCF_FACT_TICKET_MALFORMED;
    CHECK(lccf_fact_cell_init(&malformed_cell, 41U,
                              LCCF_FACT_LAYOUT_SPLIT64_64, 1U, NULL) == 0,
          "malformed cell initialization");
    CHECK(lccf_fact_try_publish(&malformed_cell, &ticket, true,
                                &counters, &won) == 0 && won,
          "malformed completion publishes failure fact");
    CHECK(lccf_fact_acquire(&malformed_cell, 41U, &fact) == 0 &&
              fact.event_kind == LCCF_FACT_EVENT_FAIL &&
              fact.error_code == EPROTO,
          "malformed completion canonical failure");

    memset(&counters, 0, sizeof(counters));
    ticket = ticket_for(LCCF_FACT_SOURCE_IOCP, 42U);
    ticket.raw_flags |= LCCF_FACT_TICKET_FAIL_PAYLOAD_PIN;
    won = false;
    CHECK(lccf_fact_cell_init(&payload_cell, 42U,
                              LCCF_FACT_LAYOUT_UNIFIED128, 1U, NULL) == 0,
          "payload cell initialization");
    CHECK(lccf_fact_try_publish(&payload_cell, &ticket, true,
                                &counters, &won) == 0 && won,
          "payload pin failure publishes conservative failure");
    CHECK(lccf_fact_acquire(&payload_cell, 42U, &fact) == 0 &&
              fact.event_kind == LCCF_FACT_EVENT_FAIL &&
              fact.payload_word == 0U,
          "failed payload is not exposed");
    CHECK(atomic_load_explicit(
              &payload_cell.references[LCCF_FACT_REF_PAYLOAD],
              memory_order_acquire) == 0U,
          "failed payload is not retained");

    memset(&counters, 0, sizeof(counters));
    ticket = ticket_for(LCCF_FACT_SOURCE_KQUEUE, 43U);
    ticket.raw_flags |= LCCF_FACT_TICKET_FAIL_MODULE_PIN;
    won = true;
    CHECK(lccf_fact_cell_init(&module_cell, 43U,
                              LCCF_FACT_LAYOUT_SPLIT96_64, 1U, NULL) == 0,
          "module cell initialization");
    CHECK(lccf_fact_try_publish(&module_cell, &ticket, true,
                                &counters, &won) == ENODEV && !won,
          "module pin failure is terminal");
    CHECK(counters.fact_build_failures == 1U &&
              counters.fact_builds == 0U,
          "module pin failure accounting");
    CHECK(lccf_fact_can_reuse(&module_cell),
          "terminal pin failure leaves cell reclaimable");
    for (index = 0U; index < LCCF_FACT_REF_COUNT; ++index) {
        CHECK(atomic_load_explicit(&module_cell.references[index],
                                   memory_order_acquire) == 0U,
              "module pin failure leaks a reference");
    }
    return 0;
}

static int test_one_winner_publishes_one_immutable_fact(void) {
    static const lccf_fact_source_t sources[] = {
        LCCF_FACT_SOURCE_LINUX_CQE,
        LCCF_FACT_SOURCE_KQUEUE,
        LCCF_FACT_SOURCE_IOCP,
    };
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters;
    lccf_fact_core_t first;
    lccf_fact_core_t acquired;
    size_t index;

    memset(&counters, 0, sizeof(counters));
    memset(&first, 0, sizeof(first));
    CHECK(lccf_fact_cell_init(&cell, 7U, LCCF_FACT_LAYOUT_SPLIT64_64,
                              3U, NULL) == 0,
          "cell initialization");

    for (index = 0U; index < sizeof(sources) / sizeof(sources[0]); ++index) {
        lccf_fact_ticket_t ticket = ticket_for(sources[index], 7U);
        bool won = true;

        ticket.owner_index = (uint32_t)index;
        CHECK(ticket.generation == 7U, "ticket construction");
        CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters,
                                    &won) == 0,
              "ticket publication");
        CHECK(won == (index == 0U), "exactly one publication winner");
        if (index == 0U) {
            CHECK(lccf_fact_acquire(&cell, 7U, &first) == 0,
                  "winner fact acquisition");
        }
    }

    CHECK(counters.claim_attempts == 3U, "claim attempt count");
    CHECK(counters.fact_builds == 1U, "fact build count");
    CHECK(counters.normalization_calls == 1U, "normalization count");
    CHECK(counters.site_lookups == 1U, "site lookup count");
    CHECK(counters.stale_losers == 2U, "stale loser count");
    CHECK(atomic_load_explicit(
              &cell.references[LCCF_FACT_REF_BACKEND],
              memory_order_relaxed) == 0U,
          "all completion tickets retire backend references");
    CHECK(lccf_fact_acquire(&cell, 7U, &acquired) == 0,
          "published fact acquisition");
    CHECK(memcmp(&first, &acquired, sizeof(first)) == 0,
          "published fact is immutable");

    {
        lccf_fact_ticket_t stale = ticket_for(LCCF_FACT_SOURCE_STOP, 6U);
        bool won = true;

        stale.event_kind = LCCF_FACT_EVENT_STOP;
        CHECK(lccf_fact_try_publish(&cell, &stale, true, &counters,
                                    &won) == LCCF_FACT_ESTALE,
              "stale generation is rejected");
        CHECK(!won, "stale generation cannot win");
        CHECK(lccf_fact_acquire(&cell, 7U, &acquired) == 0,
              "fact remains available after stale attempt");
        CHECK(memcmp(&first, &acquired, sizeof(first)) == 0,
              "stale attempt cannot mutate fact");
    }
    return 0;
}

static int test_stale_generation_cannot_retire_current_refs(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters;
    lccf_fact_ticket_t stale = ticket_for(LCCF_FACT_SOURCE_LINUX_CQE, 8U);
    bool won = true;

    memset(&counters, 0, sizeof(counters));
    CHECK(lccf_fact_cell_init(&cell, 9U, LCCF_FACT_LAYOUT_UNIFIED128,
                              1U, NULL) == 0,
          "cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &stale, true, &counters, &won) ==
              LCCF_FACT_ESTALE,
          "stale publication result");
    CHECK(!won, "stale ticket cannot win");
    CHECK(counters.generation_mismatches == 1U,
          "generation mismatch count");
    CHECK(counters.fact_builds == 0U, "stale ticket cannot build");
    CHECK(atomic_load_explicit(
              &cell.references[LCCF_FACT_REF_BACKEND],
              memory_order_relaxed) == 1U,
          "stale ticket cannot retire current backend reference");
    CHECK(atomic_load_explicit(&cell.ticket_owners[0],
                               memory_order_acquire) == 9U,
          "stale ticket cannot clear the current generation owner token");
    CHECK(lccf_fact_unpack_state(atomic_load_explicit(
              &cell.state_generation, memory_order_acquire)) ==
              LCCF_FACT_STATE_ARMED,
          "stale ticket cannot change state");
    return 0;
}

static int test_guard_is_fresh_and_fact_is_stable(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters;
    lccf_fact_ticket_t ticket = ticket_for(LCCF_FACT_SOURCE_IOCP, 4U);
    lccf_fact_guard_t guard = direct_guard(2U);
    lccf_fact_decision_t decision;
    lccf_fact_core_t published;
    lccf_fact_core_t consumed;
    bool won = false;

    memset(&counters, 0, sizeof(counters));
    CHECK(lccf_fact_cell_init(&cell, 4U, LCCF_FACT_LAYOUT_SPLIT96_64,
                              1U, NULL) == 0,
          "cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters, &won) == 0 &&
              won,
          "fact publication");
    CHECK(lccf_fact_acquire(&cell, 4U, &published) == 0,
          "published fact acquisition");

    guard.flags |= LCCF_FACT_GUARD_TRACE;
    CHECK(lccf_fact_consume(&cell, 4U, 3U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &consumed) == 0,
          "trace guard consumption");
    CHECK(decision.route == LCCF_FACT_ROUTE_QUEUE,
          "trace guard escapes to queue");
    CHECK((decision.escape_reasons & LCCF_FACT_ESCAPE_TRACE) != 0U,
          "trace escape reason");

    guard.flags &= ~LCCF_FACT_GUARD_TRACE;
    guard.consuming_shard = 1U;
    CHECK(lccf_fact_consume(&cell, 4U, 3U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &consumed) == 0,
          "wrong-shard queued consumption");
    CHECK(decision.route == LCCF_FACT_ROUTE_FORWARD &&
              decision.destination_shard == 2U,
          "queued item forwards to fresh home shard");

    guard.consuming_shard = 2U;
    CHECK(lccf_fact_consume(&cell, 4U, 3U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &consumed) == 0,
          "home-shard queued consumption");
    CHECK(decision.route == LCCF_FACT_ROUTE_QUEUE,
          "home shard begins queued execution");
    CHECK(memcmp(&published, &consumed, sizeof(published)) == 0,
          "guard changes do not rewrite fact");
    CHECK(counters.guard_rechecks == 3U, "every consumer rechecks guards");
    CHECK(counters.queue_forwards == 1U, "queue forward count");
    return 0;
}

static int test_reuse_waits_for_every_reference(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters;
    lccf_fact_ticket_t ticket = ticket_for(LCCF_FACT_SOURCE_KQUEUE, 13U);
    lccf_fact_guard_t guard = direct_guard(2U);
    lccf_fact_decision_t decision;
    lccf_fact_core_t fact;
    bool won = false;

    memset(&counters, 0, sizeof(counters));
    CHECK(lccf_fact_cell_init(&cell, 13U, LCCF_FACT_LAYOUT_UNIFIED128,
                              1U, NULL) == 0,
          "cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters, &won) == 0 &&
              won,
          "fact publication");
    CHECK(lccf_fact_consume(&cell, 13U, 3U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &fact) == 0,
          "direct consumption");
    CHECK(decision.route == LCCF_FACT_ROUTE_DIRECT, "direct route");
    CHECK(lccf_fact_module_unregister(&cell) == EBUSY,
          "published callback pins module");
    CHECK(lccf_fact_retain(&cell, LCCF_FACT_REF_EXTERNAL) == 0,
          "external reference retention");
    CHECK(lccf_fact_finish(&cell, 13U, &counters) == EBUSY,
          "external reference delays reuse");
    CHECK(counters.reuse_delays == 1U, "reuse delay count");
    CHECK(lccf_fact_unpack_state(atomic_load_explicit(
              &cell.state_generation, memory_order_acquire)) ==
              LCCF_FACT_STATE_RUNNING_DIRECT,
          "failed finish does not mutate state");
    CHECK(lccf_fact_release(&cell, LCCF_FACT_REF_EXTERNAL) == 0,
          "external reference release");
    CHECK(lccf_fact_finish(&cell, 13U, &counters) == 0,
          "reuse after all transient references retire");
    CHECK(lccf_fact_can_reuse(&cell), "retired cell is reusable");
    CHECK(lccf_fact_unpack_generation(atomic_load_explicit(
              &cell.state_generation, memory_order_acquire)) == 13U,
          "finish does not expose the next generation early");
    CHECK(lccf_fact_cell_arm(&cell, 14U, 1U) == 0,
          "explicit rearm advances generation");
    CHECK(lccf_fact_module_unregister(&cell) == 0,
          "module can unregister after callback retirement");
    return 0;
}

static int test_invalid_transitions_do_not_mutate_visible_state(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters;
    lccf_fact_ticket_t ticket = ticket_for(LCCF_FACT_SOURCE_LINUX_CQE, 21U);
    lccf_fact_guard_t guard = direct_guard(2U);
    lccf_fact_decision_t decision;
    lccf_fact_core_t before;
    lccf_fact_core_t after;
    uint64_t state_before;
    bool won = false;

    memset(&counters, 0, sizeof(counters));
    CHECK(lccf_fact_cell_init(&cell, 21U, LCCF_FACT_LAYOUT_SPLIT64_64,
                              1U, NULL) == 0,
          "cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters, &won) == 0 &&
              won,
          "fact publication");
    CHECK(lccf_fact_acquire(&cell, 21U, &before) == 0,
          "fact acquisition");
    state_before = atomic_load_explicit(&cell.state_generation,
                                        memory_order_acquire);
    CHECK(lccf_fact_yield_to_queue(&cell, 21U) == EPROTO,
          "ready cell cannot yield");
    CHECK(lccf_fact_consume(&cell, 20U, 3U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &after) ==
              LCCF_FACT_ESTALE,
          "stale consumer rejected");
    CHECK(atomic_load_explicit(&cell.state_generation,
                               memory_order_acquire) == state_before,
          "invalid transitions preserve state");
    CHECK(lccf_fact_acquire(&cell, 21U, &after) == 0,
          "fact remains acquirable");
    CHECK(memcmp(&before, &after, sizeof(before)) == 0,
          "invalid transitions preserve fact");
    return 0;
}

typedef struct publication_race_case {
    lccf_fact_cell_t *cell;
    lccf_fact_ticket_t ticket;
    _Atomic unsigned *ready;
    _Atomic bool *go;
    lccf_fact_counters_t counters;
    int rc;
    bool won;
} publication_race_case_t;

static int run_publication_race(void *opaque) {
    publication_race_case_t *race = opaque;

    atomic_fetch_add_explicit(race->ready, 1U, memory_order_release);
    while (!atomic_load_explicit(race->go, memory_order_acquire)) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    race->rc = lccf_fact_try_publish(race->cell, &race->ticket, true,
                                     &race->counters, &race->won);
    return 0;
}

static int test_publication_race_has_one_winner(void) {
    static const lccf_fact_source_t sources[] = {
        LCCF_FACT_SOURCE_LINUX_CQE,
        LCCF_FACT_SOURCE_KQUEUE,
        LCCF_FACT_SOURCE_IOCP,
    };
    lccf_fact_cell_t cell;
    publication_race_case_t cases[sizeof(sources) / sizeof(sources[0])];
    lccf_platform_thread_t
        *threads[sizeof(sources) / sizeof(sources[0])] = {NULL};
    _Atomic unsigned ready;
    _Atomic bool go;
    uint64_t builds = 0U;
    uint64_t losers = 0U;
    unsigned winners = 0U;
    size_t index;

    atomic_init(&ready, 0U);
    atomic_init(&go, false);
    CHECK(lccf_fact_cell_init(&cell, 51U, LCCF_FACT_LAYOUT_SPLIT64_64,
                              (uint32_t)(sizeof(cases) / sizeof(cases[0])), NULL) ==
              0,
          "publication race cell initialization");
    memset(cases, 0, sizeof(cases));
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        cases[index].cell = &cell;
        cases[index].ticket = ticket_for(sources[index], 51U);
        cases[index].ticket.owner_index = (uint32_t)index;
        cases[index].ready = &ready;
        cases[index].go = &go;
        CHECK(lccf_platform_thread_start(&threads[index],
                                         run_publication_race,
                                         &cases[index]) == 0,
              "publication race thread creation");
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) !=
           sizeof(cases) / sizeof(cases[0])) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    atomic_store_explicit(&go, true, memory_order_release);
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        int thread_result = -1;

        CHECK(lccf_platform_thread_join(threads[index], &thread_result) == 0 &&
                  thread_result == 0,
              "publication race thread join");
        CHECK(cases[index].rc == 0, "publication race result");
        winners += cases[index].won ? 1U : 0U;
        builds += cases[index].counters.fact_builds;
        losers += cases[index].counters.stale_losers;
    }
    CHECK(winners == 1U && builds == 1U && losers == 2U,
          "publication race must build exactly one fact");
    CHECK(atomic_load_explicit(
              &cell.references[LCCF_FACT_REF_BACKEND],
              memory_order_acquire) == 0U,
          "publication race retires every backend reference");
    return 0;
}

typedef struct consume_race_case {
    lccf_fact_cell_t *cell;
    lccf_fact_guard_t guard;
    _Atomic unsigned *ready;
    _Atomic bool *go;
    lccf_fact_counters_t counters;
    lccf_fact_decision_t decision;
    lccf_fact_core_t fact;
    int rc;
} consume_race_case_t;

static int run_consume_race(void *opaque) {
    consume_race_case_t *race = opaque;

    atomic_fetch_add_explicit(race->ready, 1U, memory_order_release);
    while (!atomic_load_explicit(race->go, memory_order_acquire)) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    race->rc = lccf_fact_consume(
        race->cell, 52U, 3U, LCCF_FACT_CONSUMER_DIRECT, &race->guard,
        &race->counters, &race->decision, &race->fact);
    return 0;
}

static int test_double_consume_race_has_one_callback(void) {
    lccf_fact_cell_t cell;
    lccf_fact_ticket_t ticket = ticket_for(LCCF_FACT_SOURCE_LINUX_CQE, 52U);
    lccf_fact_counters_t publish_counters;
    consume_race_case_t cases[2];
    lccf_platform_thread_t *threads[2] = {NULL};
    _Atomic unsigned ready;
    _Atomic bool go;
    unsigned successes = 0U;
    unsigned busy = 0U;
    bool won = false;
    size_t index;

    memset(&publish_counters, 0, sizeof(publish_counters));
    atomic_init(&ready, 0U);
    atomic_init(&go, false);
    CHECK(lccf_fact_cell_init(&cell, 52U, LCCF_FACT_LAYOUT_UNIFIED128,
                              1U, NULL) == 0,
          "consume race cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &publish_counters,
                                &won) == 0 && won,
          "consume race publication");
    memset(cases, 0, sizeof(cases));
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        cases[index].cell = &cell;
        cases[index].guard = direct_guard(2U);
        cases[index].ready = &ready;
        cases[index].go = &go;
        CHECK(lccf_platform_thread_start(&threads[index], run_consume_race,
                                         &cases[index]) == 0,
              "consume race thread creation");
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2U) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    atomic_store_explicit(&go, true, memory_order_release);
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        int thread_result = -1;

        CHECK(lccf_platform_thread_join(threads[index], &thread_result) == 0 &&
                  thread_result == 0,
              "consume race thread join");
        successes += cases[index].rc == 0 ? 1U : 0U;
        busy += cases[index].rc == EBUSY || cases[index].rc == EPROTO
                    ? 1U
                    : 0U;
    }
    CHECK(successes == 1U && busy == 1U,
          "double consume must admit one callback");
    CHECK(atomic_load_explicit(
              &cell.references[LCCF_FACT_REF_CALLBACK],
              memory_order_acquire) == 1U,
          "double consume leaves one callback reference");
    CHECK(lccf_fact_finish(&cell, 52U, &publish_counters) == 0,
          "winning callback can finish");
    return 0;
}

static int test_queue_rechecks_policy_without_rewriting_fact(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters;
    lccf_fact_ticket_t ticket = ticket_for(LCCF_FACT_SOURCE_IOCP, 61U);
    lccf_fact_guard_t guard = direct_guard(2U);
    lccf_fact_decision_t decision;
    lccf_fact_core_t published;
    lccf_fact_core_t observed;
    bool won = false;

    memset(&counters, 0, sizeof(counters));
    CHECK(lccf_fact_cell_init(&cell, 61U, LCCF_FACT_LAYOUT_SPLIT96_64,
                              1U, NULL) == 0,
          "queue policy cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters, &won) == 0 &&
              won,
          "queue policy publication");
    CHECK(lccf_fact_acquire(&cell, 61U, &published) == 0,
          "queue policy fact acquisition");

    guard.flags &= ~LCCF_FACT_GUARD_DIRECT_ENABLED;
    CHECK(lccf_fact_consume(&cell, 61U, 3U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &observed) == 0 &&
              decision.route == LCCF_FACT_ROUTE_QUEUE,
          "direct policy disable queues fact");
    CHECK(lccf_fact_consume(&cell, 61U, 3U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &observed) == 0 &&
              decision.route == LCCF_FACT_ROUTE_QUEUE,
          "direct policy disable does not disable queued callback");
    CHECK(memcmp(&published, &observed, sizeof(published)) == 0,
          "queue policy recheck preserves immutable fact");
    CHECK(lccf_fact_finish(&cell, 61U, &counters) == 0,
          "queued callback finish");
    return 0;
}

static int test_migration_forwards_then_defers_then_runs(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters;
    lccf_fact_ticket_t ticket = ticket_for(LCCF_FACT_SOURCE_KQUEUE, 62U);
    lccf_fact_guard_t guard = direct_guard(2U);
    lccf_fact_decision_t decision;
    lccf_fact_core_t original;
    lccf_fact_core_t observed;
    bool won = false;

    memset(&counters, 0, sizeof(counters));
    CHECK(lccf_fact_cell_init(&cell, 62U, LCCF_FACT_LAYOUT_SPLIT64_64,
                              1U, NULL) == 0,
          "migration cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters, &won) == 0 &&
              won,
          "migration fact publication");
    CHECK(lccf_fact_acquire(&cell, 62U, &original) == 0,
          "migration fact acquisition");
    guard.flags |= LCCF_FACT_GUARD_MIGRATING;
    guard.current_home_shard = 3U;
    CHECK(lccf_fact_consume(&cell, 62U, 3U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &observed) == 0 &&
              decision.route == LCCF_FACT_ROUTE_QUEUE,
          "migration escapes direct execution");
    CHECK(lccf_fact_consume(&cell, 62U, 3U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &observed) == 0 &&
              decision.route == LCCF_FACT_ROUTE_FORWARD &&
              decision.destination_shard == 3U,
          "stale shard forwards queued fact");
    guard.consuming_shard = 3U;
    CHECK(lccf_fact_consume(&cell, 62U, 3U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &observed) == 0 &&
              decision.route == LCCF_FACT_ROUTE_DEFER &&
              (decision.escape_reasons & LCCF_FACT_ESCAPE_MIGRATION) != 0U,
          "active migration defers callback");
    guard.flags &= ~LCCF_FACT_GUARD_MIGRATING;
    CHECK(lccf_fact_consume(&cell, 62U, 3U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &observed) == 0 &&
              decision.route == LCCF_FACT_ROUTE_QUEUE,
          "settled migration runs on new home shard");
    CHECK(memcmp(&original, &observed, sizeof(original)) == 0,
          "migration guard never rewrites captured fact");
    CHECK(lccf_fact_finish(&cell, 62U, &counters) == 0,
          "migrated callback finish");
    return 0;
}

static int test_yield_transfers_callback_reference_to_queue(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters;
    lccf_fact_ticket_t ticket = ticket_for(LCCF_FACT_SOURCE_EXTERNAL, 63U);
    lccf_fact_guard_t guard = direct_guard(2U);
    lccf_fact_decision_t decision;
    lccf_fact_core_t fact;
    bool won = false;

    ticket.event_kind = LCCF_FACT_EVENT_EXTERNAL;
    memset(&counters, 0, sizeof(counters));
    CHECK(lccf_fact_cell_init(&cell, 63U, LCCF_FACT_LAYOUT_UNIFIED128,
                              1U, NULL) == 0,
          "yield cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters, &won) == 0 &&
              won,
          "yield fact publication");
    CHECK(lccf_fact_consume(&cell, 63U, 3U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &fact) == 0 &&
              decision.route == LCCF_FACT_ROUTE_DIRECT,
          "yield direct admission");
    CHECK(lccf_fact_yield_to_queue(&cell, 63U) == 0,
          "callback yields to queue");
    CHECK(atomic_load_explicit(&cell.references[LCCF_FACT_REF_CALLBACK],
                               memory_order_acquire) == 0U &&
              atomic_load_explicit(&cell.references[LCCF_FACT_REF_QUEUE],
                                   memory_order_acquire) == 1U,
          "yield transfers callback ownership to queue");
    CHECK(lccf_fact_consume(&cell, 63U, 3U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &fact) == 0 &&
              decision.route == LCCF_FACT_ROUTE_QUEUE,
          "yielded callback resumes from queue");
    CHECK(lccf_fact_finish(&cell, 63U, &counters) == 0 &&
              lccf_fact_can_reuse(&cell) &&
              lccf_fact_cell_arm(&cell, 64U, 1U) == 0,
          "yielded callback retires cleanly");
    return 0;
}

static int test_direct_guard_escape_matrix(void) {
    static const struct {
        uint64_t clear_flags;
        uint64_t set_flags;
        bool zero_budget;
        bool wrong_shard;
        uint64_t expected_reason;
    } cases[] = {
        {LCCF_FACT_GUARD_DIRECT_ENABLED, 0U, false, false,
         LCCF_FACT_ESCAPE_MODULE_POLICY},
        {LCCF_FACT_GUARD_MODULE_ENABLED, 0U, false, false,
         LCCF_FACT_ESCAPE_MODULE_POLICY},
        {LCCF_FACT_GUARD_BACKEND_CAPABLE, 0U, false, false,
         LCCF_FACT_ESCAPE_BACKEND_CAPABILITY},
        {0U, LCCF_FACT_GUARD_STOP, false, false,
         LCCF_FACT_ESCAPE_STOP},
        {0U, LCCF_FACT_GUARD_FAIRNESS_DUE, false, false,
         LCCF_FACT_ESCAPE_FAIRNESS},
        {0U, LCCF_FACT_GUARD_TRACE, false, false,
         LCCF_FACT_ESCAPE_TRACE},
        {0U, LCCF_FACT_GUARD_SHARD_PAUSED, false, false,
         LCCF_FACT_ESCAPE_SHARD_STATE},
        {0U, LCCF_FACT_GUARD_SHARD_OFFLINE, false, false,
         LCCF_FACT_ESCAPE_SHARD_STATE},
        {0U, LCCF_FACT_GUARD_CALLBACK_ACTIVE, false, false,
         LCCF_FACT_ESCAPE_CALLBACK_ACTIVE},
        {0U, LCCF_FACT_GUARD_QUEUE_PRESSURE, false, false,
         LCCF_FACT_ESCAPE_QUEUE_PRESSURE},
        {0U, LCCF_FACT_GUARD_MIGRATING, false, false,
         LCCF_FACT_ESCAPE_MIGRATION},
        {0U, 0U, true, false, LCCF_FACT_ESCAPE_BUDGET},
        {0U, 0U, false, true, LCCF_FACT_ESCAPE_WRONG_SHARD},
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const uint64_t generation = UINT64_C(80) + index;
        lccf_fact_cell_t cell;
        lccf_fact_counters_t counters;
        lccf_fact_ticket_t ticket =
            ticket_for(LCCF_FACT_SOURCE_LINUX_CQE, generation);
        lccf_fact_guard_t guard = direct_guard(2U);
        lccf_fact_decision_t decision;
        lccf_fact_core_t before;
        lccf_fact_core_t after;
        bool won = false;

        memset(&counters, 0, sizeof(counters));
        CHECK(lccf_fact_cell_init(&cell, generation,
                                  LCCF_FACT_LAYOUT_SPLIT64_64, 1U, NULL) == 0,
              "guard matrix cell initialization");
        CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters,
                                    &won) == 0 && won,
              "guard matrix publication");
        CHECK(lccf_fact_acquire(&cell, generation, &before) == 0,
              "guard matrix fact acquisition");
        guard.flags &= ~cases[index].clear_flags;
        guard.flags |= cases[index].set_flags;
        if (cases[index].zero_budget) {
            guard.budget_remaining = 0U;
        }
        if (cases[index].wrong_shard) {
            guard.consuming_shard = 1U;
        }
        CHECK(lccf_fact_consume(&cell, generation, 3U,
                                LCCF_FACT_CONSUMER_DIRECT, &guard,
                                &counters, &decision, &after) == 0,
              "guard matrix direct consumption");
        CHECK(decision.route == LCCF_FACT_ROUTE_QUEUE &&
                  decision.escape_reasons == cases[index].expected_reason,
              "guard matrix exact escape reason");
        CHECK(memcmp(&before, &after, sizeof(before)) == 0,
              "guard matrix cannot rewrite fact");
    }
    return 0;
}

static int test_queued_guard_recheck_matrix(void) {
    static const struct {
        uint64_t clear_flags;
        uint64_t set_flags;
        bool zero_budget;
        lccf_fact_route_t expected_route;
        uint64_t expected_reason;
    } cases[] = {
        {LCCF_FACT_GUARD_MODULE_ENABLED, 0U, false,
         LCCF_FACT_ROUTE_DEFER, LCCF_FACT_ESCAPE_MODULE_POLICY},
        {LCCF_FACT_GUARD_BACKEND_CAPABLE, 0U, false,
         LCCF_FACT_ROUTE_DEFER, LCCF_FACT_ESCAPE_BACKEND_CAPABILITY},
        {0U, LCCF_FACT_GUARD_STOP, false,
         LCCF_FACT_ROUTE_DEFER, LCCF_FACT_ESCAPE_STOP},
        {0U, LCCF_FACT_GUARD_MIGRATING, false,
         LCCF_FACT_ROUTE_DEFER, LCCF_FACT_ESCAPE_MIGRATION},
        {0U, LCCF_FACT_GUARD_CALLBACK_ACTIVE, false,
         LCCF_FACT_ROUTE_DEFER, LCCF_FACT_ESCAPE_CALLBACK_ACTIVE},
        {0U, LCCF_FACT_GUARD_SHARD_PAUSED, false,
         LCCF_FACT_ROUTE_DEFER, LCCF_FACT_ESCAPE_SHARD_STATE},
        {0U, LCCF_FACT_GUARD_SHARD_OFFLINE, false,
         LCCF_FACT_ROUTE_DEFER, LCCF_FACT_ESCAPE_SHARD_STATE},
        {LCCF_FACT_GUARD_DIRECT_ENABLED, LCCF_FACT_GUARD_TRACE, false,
         LCCF_FACT_ROUTE_QUEUE, 0U},
        {0U, LCCF_FACT_GUARD_FAIRNESS_DUE, false,
         LCCF_FACT_ROUTE_QUEUE, 0U},
        {0U, LCCF_FACT_GUARD_QUEUE_PRESSURE, false,
         LCCF_FACT_ROUTE_QUEUE, 0U},
        {0U, 0U, true, LCCF_FACT_ROUTE_QUEUE, 0U},
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const uint64_t generation = UINT64_C(100) + index;
        lccf_fact_cell_t cell;
        lccf_fact_counters_t counters;
        lccf_fact_ticket_t ticket =
            ticket_for(LCCF_FACT_SOURCE_IOCP, generation);
        lccf_fact_guard_t guard = direct_guard(2U);
        lccf_fact_decision_t decision;
        lccf_fact_core_t original;
        lccf_fact_core_t observed;
        bool won = false;

        memset(&counters, 0, sizeof(counters));
        CHECK(lccf_fact_cell_init(&cell, generation,
                                  LCCF_FACT_LAYOUT_UNIFIED128, 1U, NULL) == 0,
              "queued guard cell initialization");
        CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters,
                                    &won) == 0 && won,
              "queued guard publication");
        CHECK(lccf_fact_acquire(&cell, generation, &original) == 0,
              "queued guard fact acquisition");
        guard.flags |= LCCF_FACT_GUARD_TRACE;
        CHECK(lccf_fact_consume(&cell, generation, 3U,
                                LCCF_FACT_CONSUMER_DIRECT, &guard,
                                &counters, &decision, &observed) == 0 &&
                  decision.route == LCCF_FACT_ROUTE_QUEUE,
              "queued guard setup");
        guard = direct_guard(2U);
        guard.flags &= ~cases[index].clear_flags;
        guard.flags |= cases[index].set_flags;
        if (cases[index].zero_budget) {
            guard.budget_remaining = 0U;
        }
        CHECK(lccf_fact_consume(&cell, generation, 3U,
                                LCCF_FACT_CONSUMER_QUEUE, &guard,
                                &counters, &decision, &observed) == 0 &&
                  decision.route == cases[index].expected_route &&
                  decision.escape_reasons == cases[index].expected_reason,
              "queued guard route and reason");
        CHECK(memcmp(&original, &observed, sizeof(original)) == 0,
              "queued guard cannot rewrite fact");
        if (decision.route == LCCF_FACT_ROUTE_DEFER) {
            guard = direct_guard(2U);
            CHECK(lccf_fact_consume(&cell, generation, 3U,
                                    LCCF_FACT_CONSUMER_QUEUE, &guard,
                                    &counters, &decision, &observed) == 0 &&
                      decision.route == LCCF_FACT_ROUTE_QUEUE,
                  "deferred queued fact recovers when guard clears");
        }
        CHECK(lccf_fact_finish(&cell, generation, &counters) == 0,
              "queued guard callback retirement");
    }
    return 0;
}

static int test_offline_destination_defers_before_forward(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters;
    lccf_fact_ticket_t ticket = ticket_for(LCCF_FACT_SOURCE_KQUEUE, 120U);
    lccf_fact_guard_t guard = direct_guard(2U);
    lccf_fact_decision_t decision;
    lccf_fact_core_t fact;
    bool won = false;

    memset(&counters, 0, sizeof(counters));
    CHECK(lccf_fact_cell_init(&cell, 120U, LCCF_FACT_LAYOUT_SPLIT96_64,
                              1U, NULL) == 0,
          "offline destination cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters, &won) == 0 &&
              won,
          "offline destination publication");
    guard.flags |= LCCF_FACT_GUARD_TRACE;
    CHECK(lccf_fact_consume(&cell, 120U, 3U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &fact) == 0,
          "offline destination queue setup");
    guard = direct_guard(2U);
    guard.consuming_shard = 1U;
    guard.flags |= LCCF_FACT_GUARD_SHARD_OFFLINE;
    CHECK(lccf_fact_consume(&cell, 120U, 3U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &fact) == 0 &&
              decision.route == LCCF_FACT_ROUTE_DEFER &&
              decision.escape_reasons == LCCF_FACT_ESCAPE_SHARD_STATE,
          "offline destination must not receive forward");
    guard = direct_guard(2U);
    CHECK(lccf_fact_consume(&cell, 120U, 3U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &fact) == 0 &&
              decision.route == LCCF_FACT_ROUTE_QUEUE,
          "offline destination resumes when online");
    CHECK(lccf_fact_finish(&cell, 120U, &counters) == 0,
          "offline destination callback retirement");
    return 0;
}

static int test_external_storage_and_resolved_site_are_real(void) {
    lccf_fact_cell_t cell;
    lccf_fact_core_t external_fact;
    lccf_fact_counters_t counters = {0};
    lccf_fact_ticket_t ticket =
        ticket_for(LCCF_FACT_SOURCE_LINUX_CQE, 130U);
    lccf_fact_core_t observed;
    unsigned calls = 0U;
    bool won = false;

    memset(&external_fact, 0xa5, sizeof(external_fact));
    CHECK(lccf_fact_cell_init(&cell, 130U,
                              LCCF_FACT_LAYOUT_SPLIT64_64, 1U,
                              &external_fact) == 0,
          "external fact storage initialization");
    CHECK(lccf_fact_storage(&cell) == &external_fact,
          "cell must use bound layout storage");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters,
                                &won) == 0 && won,
          "external fact publication");
    CHECK(lccf_fact_acquire(&cell, 130U, &observed) == 0,
          "external fact acquisition");
    CHECK(memcmp(&external_fact, &observed, sizeof(observed)) == 0,
          "publication must write the bound layout storage");
    CHECK(observed.resolved_site == TEST_SITE_TABLE[3] &&
              counters.site_lookups == 1U,
          "publication must retain one resolved site descriptor");
    CHECK(lccf_fact_invoke(&observed, &calls) == 0 && calls == 1U,
          "consumer must invoke the retained descriptor");
    return 0;
}

static int test_rearm_rejects_generation_rollback(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters = {0};
    lccf_fact_ticket_t ticket =
        ticket_for(LCCF_FACT_SOURCE_IOCP, 140U);
    lccf_fact_guard_t guard = direct_guard(2U);
    lccf_fact_decision_t decision;
    lccf_fact_core_t fact;
    uint64_t terminal_word;
    bool won = false;

    CHECK(lccf_fact_cell_init(&cell, 140U,
                              LCCF_FACT_LAYOUT_UNIFIED128, 1U,
                              NULL) == 0,
          "rollback cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters,
                                &won) == 0 && won,
          "rollback fact publication");
    CHECK(lccf_fact_consume(&cell, 140U, 3U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &fact) == 0 &&
              decision.route == LCCF_FACT_ROUTE_DIRECT,
          "rollback direct admission");
    CHECK(lccf_fact_finish(&cell, 140U, &counters) == 0,
          "rollback terminal retirement");
    terminal_word = atomic_load_explicit(&cell.state_generation,
                                         memory_order_acquire);
    CHECK(lccf_fact_cell_arm(&cell, 139U, 1U) == LCCF_FACT_ESTALE,
          "generation rollback must be rejected");
    CHECK(atomic_load_explicit(&cell.state_generation,
                               memory_order_acquire) == terminal_word,
          "failed rollback must preserve terminal state");
    CHECK(lccf_fact_cell_arm(&cell, 141U, 1U) == 0,
          "strictly newer generation can rearm");
    return 0;
}

static int test_abort_releases_every_owned_reference(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters = {0};
    lccf_fact_ticket_t ticket =
        ticket_for(LCCF_FACT_SOURCE_KQUEUE, 150U);
    lccf_fact_guard_t guard = direct_guard(2U);
    lccf_fact_decision_t decision;
    lccf_fact_core_t fact;
    bool won = false;
    size_t kind;

    CHECK(lccf_fact_cell_init(&cell, 150U,
                              LCCF_FACT_LAYOUT_SPLIT96_64, 1U,
                              NULL) == 0,
          "abort cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters,
                                &won) == 0 && won,
          "abort fact publication");
    CHECK(lccf_fact_consume(&cell, 150U, 3U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &fact) == 0 &&
              decision.route == LCCF_FACT_ROUTE_DIRECT,
          "abort direct admission");
    CHECK(lccf_fact_abort(&cell, 150U) == 0,
          "callback failure teardown");
    CHECK(lccf_fact_unpack_state(atomic_load_explicit(
              &cell.state_generation, memory_order_acquire)) ==
              LCCF_FACT_STATE_TERMINAL,
          "abort publishes terminal state");
    for (kind = 0U; kind < LCCF_FACT_REF_COUNT; ++kind) {
        CHECK(atomic_load_explicit(&cell.references[kind],
                                   memory_order_acquire) == 0U,
              "abort must release every reference kind");
    }
    CHECK(lccf_fact_can_reuse(&cell),
          "aborted terminal cell is reusable");
    return 0;
}

typedef struct acquire_rearm_race {
    lccf_fact_cell_t *cell;
    _Atomic uint64_t generation;
    _Atomic bool stop;
    _Atomic unsigned failures;
} acquire_rearm_race_t;

static int run_acquire_rearm_race(void *opaque) {
    acquire_rearm_race_t *race = opaque;

    while (!atomic_load_explicit(&race->stop, memory_order_acquire)) {
        const uint64_t generation = atomic_load_explicit(
            &race->generation, memory_order_acquire);
        lccf_fact_core_t fact;
        const int rc = lccf_fact_acquire(
            race->cell, generation, &fact);

        if (rc == 0) {
            if (fact.generation != generation ||
                fact.resolved_site == NULL ||
                fact.resolved_site->logical_index != fact.site_index) {
                atomic_fetch_add_explicit(
                    &race->failures, 1U, memory_order_relaxed);
            }
        } else if (rc != LCCF_FACT_ESTALE && rc != EAGAIN &&
                   rc != EBUSY) {
            atomic_fetch_add_explicit(
                &race->failures, 1U, memory_order_relaxed);
        }
    }
    return 0;
}

static int test_acquire_finish_rearm_race(void) {
    enum { GENERATIONS = 2000 };
    lccf_fact_cell_t cell;
    lccf_fact_core_t external_fact;
    acquire_rearm_race_t race;
    lccf_platform_thread_t *thread = NULL;
    uint64_t generation;

    memset(&race, 0, sizeof(race));
    CHECK(lccf_fact_cell_init(&cell, 1U,
                              LCCF_FACT_LAYOUT_SPLIT64_64, 1U,
                              &external_fact) == 0,
          "acquire/rearm race initialization");
    race.cell = &cell;
    atomic_init(&race.generation, 1U);
    atomic_init(&race.stop, false);
    atomic_init(&race.failures, 0U);
    CHECK(lccf_platform_thread_start(&thread, run_acquire_rearm_race,
                                     &race) == 0,
          "acquire/rearm race thread creation");

    for (generation = 1U; generation <= GENERATIONS; ++generation) {
        lccf_fact_ticket_t ticket =
            ticket_for(LCCF_FACT_SOURCE_LINUX_CQE, generation);
        lccf_fact_counters_t counters = {0};
        lccf_fact_guard_t guard = direct_guard(2U);
        lccf_fact_decision_t decision;
        lccf_fact_core_t fact;
        bool won = false;

        CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters,
                                    &won) == 0 && won,
              "acquire/rearm race publication");
        atomic_store_explicit(&race.generation, generation,
                              memory_order_release);
        CHECK(lccf_fact_consume(&cell, generation, 3U,
                                LCCF_FACT_CONSUMER_DIRECT, &guard,
                                &counters, &decision, &fact) == 0 &&
                  decision.route == LCCF_FACT_ROUTE_DIRECT,
              "acquire/rearm race consumption");
        CHECK(lccf_fact_finish(&cell, generation, &counters) == 0,
              "acquire/rearm race retirement");
        if (generation != GENERATIONS) {
            CHECK(lccf_fact_cell_arm(&cell, generation + 1U, 1U) == 0,
                  "acquire/rearm race next generation");
        }
    }
    atomic_store_explicit(&race.stop, true, memory_order_release);
    {
        int thread_result = -1;

        CHECK(lccf_platform_thread_join(thread, &thread_result) == 0 &&
                  thread_result == 0,
              "acquire/rearm race thread join");
    }
    CHECK(atomic_load_explicit(&race.failures,
                               memory_order_acquire) == 0U,
          "acquire/rearm race observed a torn fact");
    CHECK(lccf_fact_can_reuse(&cell),
          "acquire/rearm race retires cleanly");
    return 0;
}

static int test_configured_representations_share_protocol(void) {
    static const lccf_representation_t representations[] = {
        LCCF_REP_CANONICAL_HELPER,
        LCCF_REP_SHARED_EVENT,
        LCCF_REP_FULL_FACT,
    };
    static const uint64_t expected_normalizations[] = {4U, 1U, 1U};
    static const uint64_t expected_lookups[] = {4U, 4U, 2U};
    size_t index;

    for (index = 0U;
         index < sizeof(representations) / sizeof(representations[0]);
         ++index) {
        union {
            lccf_event_core_t event;
            lccf_fact_core_t fact;
        } storage;
        unsigned char published[sizeof(storage)];
        lccf_fact_cell_t cell;
        lccf_fact_ticket_t ticket = ticket_for(
            LCCF_FACT_SOURCE_LINUX_CQE, 201U + index);
        lccf_fact_counters_t counters = {0};
        lccf_fact_core_t materialized;
        lccf_fact_core_t acquired;
        lccf_fact_decision_t decision;
        lccf_fact_guard_t guard = direct_guard(2U);
        void *storage_pointer =
            representations[index] == LCCF_REP_CANONICAL_HELPER
                ? NULL
                : (void *)&storage;
        const size_t sidecar_bytes = lccf_representation_sidecar_bytes(
            representations[index]);
        bool won = false;

        memset(&storage, 0xa5, sizeof(storage));
        CHECK(lccf_fact_cell_init_representation(
                  &cell, ticket.generation,
                  LCCF_FACT_LAYOUT_SPLIT64_64, 1U,
                  representations[index], storage_pointer) == 0,
              "configured representation initialization");
        CHECK(lccf_fact_try_publish_configured(
                  &cell, &ticket, &counters, &won) == 0 && won,
              "configured representation publication");
        if (sidecar_bytes != 0U) {
            memcpy(published, &storage, sidecar_bytes);
        }
        if (representations[index] == LCCF_REP_CANONICAL_HELPER) {
            CHECK(lccf_fact_acquire(
                      &cell, ticket.generation, &acquired) == ENODATA,
                  "canonical helper has no persistent acquisition");
        } else {
            CHECK(lccf_fact_acquire(
                      &cell, ticket.generation, &acquired) == 0 &&
                      acquired.resolved_site == TEST_SITE_TABLE[3],
                  "persistent representation acquisition");
        }
        CHECK(lccf_fact_materialize(
                  &cell, ticket.generation, 3U, &counters,
                  &materialized) == 0 &&
                  materialized.resolved_site == TEST_SITE_TABLE[3],
              "first initial-site materialization");
        CHECK(lccf_fact_materialize(
                  &cell, ticket.generation, 3U, &counters,
                  &materialized) == 0 &&
                  materialized.resolved_site == TEST_SITE_TABLE[3],
              "second initial-site materialization");
        CHECK(lccf_fact_materialize(
                  &cell, ticket.generation, 5U, &counters,
                  &materialized) == 0 &&
                  materialized.resolved_site == TEST_SITE_TABLE[5],
              "changed-site materialization");
        CHECK(lccf_fact_consume(
                  &cell, ticket.generation, 3U,
                  LCCF_FACT_CONSUMER_DIRECT, &guard, &counters,
                  &decision, &materialized) == 0 &&
                  decision.route == LCCF_FACT_ROUTE_DIRECT &&
                  materialized.resolved_site == TEST_SITE_TABLE[3],
              "configured representation direct consume");
        CHECK(counters.normalization_calls ==
                  expected_normalizations[index] &&
                  counters.site_lookups == expected_lookups[index],
              "configured representation work equation");
        if (sidecar_bytes != 0U) {
            CHECK(memcmp(published, &storage, sidecar_bytes) == 0,
                  "configured representation storage is immutable");
        }
        CHECK(lccf_fact_finish(
                  &cell, ticket.generation, &counters) == 0,
              "configured representation finish");
        CHECK(lccf_fact_can_reuse(&cell),
              "configured representation retires every reference");
    }
    return 0;
}

static int test_loser_cannot_reconfigure_winner_representation(void) {
    lccf_fact_cell_t cell;
    lccf_fact_counters_t counters = {0};
    lccf_fact_ticket_t winner = ticket_for(
        LCCF_FACT_SOURCE_LINUX_CQE, 211U);
    lccf_fact_ticket_t loser = ticket_for(
        LCCF_FACT_SOURCE_KQUEUE, 211U);
    lccf_fact_core_t before;
    lccf_fact_core_t after;
    bool won = false;

    loser.owner_index = 1U;
    CHECK(lccf_fact_cell_init(
              &cell, 211U, LCCF_FACT_LAYOUT_SPLIT64_64, 2U, NULL) == 0,
          "loser reconfiguration cell initialization");
    CHECK(lccf_fact_try_publish(
              &cell, &winner, true, &counters, &won) == 0 && won,
          "persistent representation winner publication");
    CHECK(lccf_fact_acquire(&cell, 211U, &before) == 0,
          "winner representation acquisition");
    won = true;
    CHECK(lccf_fact_try_publish(
              &cell, &loser, false, &counters, &won) == 0 && !won,
          "canonical-helper loser retirement");
    CHECK(atomic_load_explicit(
              &cell.representation, memory_order_acquire) ==
              LCCF_REP_FULL_FACT,
          "loser cannot replace winner representation");
    CHECK(lccf_fact_acquire(&cell, 211U, &after) == 0 &&
              memcmp(&before, &after, sizeof(before)) == 0,
          "loser cannot invalidate winner storage");
    return 0;
}

int main(void) {
    static const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"layout contracts", test_layout_contracts},
        {"platform normalization",
         test_platform_adapters_normalize_equivalent_events},
        {"shared materialization",
         test_shared_fact_removes_repeated_materialization_work},
        {"failure-path balance",
         test_failure_paths_publish_or_retire_without_leaks},
        {"single immutable publication",
         test_one_winner_publishes_one_immutable_fact},
        {"stale generation isolation",
         test_stale_generation_cannot_retire_current_refs},
        {"fresh guards stable facts", test_guard_is_fresh_and_fact_is_stable},
        {"reference-bound reuse", test_reuse_waits_for_every_reference},
        {"invalid transition atomicity",
         test_invalid_transitions_do_not_mutate_visible_state},
        {"publication race", test_publication_race_has_one_winner},
        {"double consume race", test_double_consume_race_has_one_callback},
        {"queue policy recheck",
         test_queue_rechecks_policy_without_rewriting_fact},
        {"migration routing", test_migration_forwards_then_defers_then_runs},
        {"yield ownership transfer",
         test_yield_transfers_callback_reference_to_queue},
        {"direct guard escape matrix", test_direct_guard_escape_matrix},
        {"queued guard recheck matrix", test_queued_guard_recheck_matrix},
        {"offline destination ordering",
         test_offline_destination_defers_before_forward},
        {"physical fact storage and site resolution",
         test_external_storage_and_resolved_site_are_real},
        {"generation rollback rejection",
         test_rearm_rejects_generation_rollback},
        {"callback failure teardown",
         test_abort_releases_every_owned_reference},
        {"acquire/finish/rearm race",
         test_acquire_finish_rearm_race},
        {"configured representation protocol",
         test_configured_representations_share_protocol},
        {"winner representation isolation",
         test_loser_cannot_reconfigure_winner_representation},
    };
    size_t index;

    for (index = 0U; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (tests[index].run() != 0) {
            fprintf(stderr, "[test_lccf_fact] FAIL: %s\n", tests[index].name);
            return 1;
        }
        printf("[test_lccf_fact] PASS: %s\n", tests[index].name);
    }
    return 0;
}
