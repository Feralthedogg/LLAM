/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lccf_fact.h"

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

static lccf_fact_ticket_t ticket_for(lccf_fact_source_t source,
                                     uint64_t generation) {
    lccf_fact_ticket_t ticket;
    const int rc = lccf_fact_ticket_from_logical(
        source, LCCF_FACT_EVENT_IO, generation, 37, EAGAIN,
        UINT64_C(0xfeedface), 3U, 8U, 2U, 5U, &ticket);

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
    CHECK(alignof(lccf_fact_core_t) >= alignof(uint64_t),
          "fact core alignment");
    CHECK(alignof(lccf_fact_cell_t) >= alignof(uint64_t),
          "fact cell alignment");
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
                              3U) == 0,
          "cell initialization");

    for (index = 0U; index < sizeof(sources) / sizeof(sources[0]); ++index) {
        lccf_fact_ticket_t ticket = ticket_for(sources[index], 7U);
        bool won = true;

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
                                    &won) == ESTALE,
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
                              1U) == 0,
          "cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &stale, true, &counters, &won) ==
              ESTALE,
          "stale publication result");
    CHECK(!won, "stale ticket cannot win");
    CHECK(counters.generation_mismatches == 1U,
          "generation mismatch count");
    CHECK(counters.fact_builds == 0U, "stale ticket cannot build");
    CHECK(atomic_load_explicit(
              &cell.references[LCCF_FACT_REF_BACKEND],
              memory_order_relaxed) == 1U,
          "stale ticket cannot retire current backend reference");
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
                              1U) == 0,
          "cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters, &won) == 0 &&
              won,
          "fact publication");
    CHECK(lccf_fact_acquire(&cell, 4U, &published) == 0,
          "published fact acquisition");

    guard.flags |= LCCF_FACT_GUARD_TRACE;
    CHECK(lccf_fact_consume(&cell, 4U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &consumed) == 0,
          "trace guard consumption");
    CHECK(decision.route == LCCF_FACT_ROUTE_QUEUE,
          "trace guard escapes to queue");
    CHECK((decision.escape_reasons & LCCF_FACT_ESCAPE_TRACE) != 0U,
          "trace escape reason");

    guard.flags &= ~LCCF_FACT_GUARD_TRACE;
    guard.consuming_shard = 1U;
    CHECK(lccf_fact_consume(&cell, 4U, LCCF_FACT_CONSUMER_QUEUE,
                            &guard, &counters, &decision, &consumed) == 0,
          "wrong-shard queued consumption");
    CHECK(decision.route == LCCF_FACT_ROUTE_FORWARD &&
              decision.destination_shard == 2U,
          "queued item forwards to fresh home shard");

    guard.consuming_shard = 2U;
    CHECK(lccf_fact_consume(&cell, 4U, LCCF_FACT_CONSUMER_QUEUE,
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
                              1U) == 0,
          "cell initialization");
    CHECK(lccf_fact_try_publish(&cell, &ticket, true, &counters, &won) == 0 &&
              won,
          "fact publication");
    CHECK(lccf_fact_consume(&cell, 13U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &fact) == 0,
          "direct consumption");
    CHECK(decision.route == LCCF_FACT_ROUTE_DIRECT, "direct route");
    CHECK(lccf_fact_module_unregister(&cell) == EBUSY,
          "published callback pins module");
    CHECK(lccf_fact_retain(&cell, LCCF_FACT_REF_EXTERNAL) == 0,
          "external reference retention");
    CHECK(lccf_fact_finish(&cell, 13U, false, 14U, &counters) == EBUSY,
          "external reference delays reuse");
    CHECK(counters.reuse_delays == 1U, "reuse delay count");
    CHECK(lccf_fact_unpack_state(atomic_load_explicit(
              &cell.state_generation, memory_order_acquire)) ==
              LCCF_FACT_STATE_RUNNING_DIRECT,
          "failed finish does not mutate state");
    CHECK(lccf_fact_release(&cell, LCCF_FACT_REF_EXTERNAL) == 0,
          "external reference release");
    CHECK(lccf_fact_finish(&cell, 13U, false, 14U, &counters) == 0,
          "reuse after all transient references retire");
    CHECK(lccf_fact_can_reuse(&cell), "rearmed cell is reusable");
    CHECK(lccf_fact_unpack_generation(atomic_load_explicit(
              &cell.state_generation, memory_order_acquire)) == 14U,
          "rearm advances generation");
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
                              1U) == 0,
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
    CHECK(lccf_fact_consume(&cell, 20U, LCCF_FACT_CONSUMER_DIRECT,
                            &guard, &counters, &decision, &after) == ESTALE,
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

int main(void) {
    static const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"layout contracts", test_layout_contracts},
        {"single immutable publication",
         test_one_winner_publishes_one_immutable_fact},
        {"stale generation isolation",
         test_stale_generation_cannot_retire_current_refs},
        {"fresh guards stable facts", test_guard_is_fresh_and_fact_is_stable},
        {"reference-bound reuse", test_reuse_waits_for_every_reference},
        {"invalid transition atomicity",
         test_invalid_transitions_do_not_mutate_visible_state},
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
