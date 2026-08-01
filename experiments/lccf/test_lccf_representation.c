/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lccf_fact.h"
#include "lccf_representation.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, message)                                         \
    do {                                                                  \
        if (!(condition)) {                                               \
            fprintf(stderr, "[test_lccf_representation] FAIL: %s\n",     \
                    (message));                                           \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static int invoke_site(
    const lccf_fact_site_descriptor_t *descriptor,
    const lccf_fact_core_t *fact,
    void *context) {
    uint32_t *observed = context;

    if (descriptor == NULL || fact == NULL || observed == NULL ||
        descriptor->logical_index != fact->site_index) {
        return LCCF_FACT_ESTALE;
    }
    *observed = descriptor->logical_index;
    return 0;
}

static void make_sites(
    lccf_fact_site_descriptor_t descriptors[8],
    const lccf_fact_site_descriptor_t *table[8]) {
    uint32_t index;

    for (index = 0U; index < 8U; ++index) {
        descriptors[index].invoke = invoke_site;
        descriptors[index].logical_index = index;
        descriptors[index].reserved = 0U;
        table[index] = &descriptors[index];
    }
}

static int make_ticket(
    const lccf_fact_site_descriptor_t *const table[8],
    lccf_fact_ticket_t *ticket) {
    return lccf_fact_ticket_from_logical(
        LCCF_FACT_SOURCE_LINUX_CQE, LCCF_FACT_EVENT_IO,
        UINT64_C(41), INT64_C(73), 0, UINT64_C(0x123456789abcdef0),
        2U, 8U, 3U, 5U, 0U, table, ticket);
}

static bool event_equal(const lccf_fact_core_t *left,
                        const lccf_fact_core_t *right) {
    return left->generation == right->generation &&
           left->fact_id != 0U && left->fact_id == right->fact_id &&
           left->stable_flags == right->stable_flags &&
           left->result == right->result &&
           left->payload_word == right->payload_word &&
           left->error_code == right->error_code &&
           left->captured_home_shard == right->captured_home_shard &&
           left->source_node == right->source_node &&
           left->event_kind == right->event_kind &&
           left->source_kind == right->source_kind &&
           left->site_index == right->site_index &&
           left->resolved_site == right->resolved_site;
}

static int test_layout_contracts(void) {
    CHECK(sizeof(lccf_event_core_t) == 48U,
          "event core is exactly 48 bytes");
    CHECK(lccf_representation_sidecar_bytes(
              LCCF_REP_CANONICAL_HELPER) == 0U,
          "helper has no sidecar");
    CHECK(lccf_representation_sidecar_bytes(
              LCCF_REP_SHARED_EVENT) == 48U,
          "event sidecar is 48 bytes");
    CHECK(lccf_representation_sidecar_bytes(
              LCCF_REP_FULL_FACT) == 64U,
          "full fact sidecar is 64 bytes");
    CHECK(lccf_representation_sidecar_bytes(LCCF_REP_COUNT) == 0U,
          "invalid representation has no advertised storage");
    return 0;
}

static int test_work_separation_and_immutability(void) {
    lccf_fact_site_descriptor_t descriptors[8];
    const lccf_fact_site_descriptor_t *table[8];
    lccf_fact_ticket_t ticket;
    lccf_event_core_t event_storage;
    lccf_event_core_t event_snapshot;
    lccf_fact_core_t fact_storage;
    lccf_fact_core_t fact_snapshot;
    lccf_fact_core_t helper_fact;
    lccf_fact_core_t event_fact;
    lccf_fact_core_t full_fact;
    lccf_fact_counters_t helper_counters = {0};
    lccf_fact_counters_t event_counters = {0};
    lccf_fact_counters_t full_counters = {0};

    make_sites(descriptors, table);
    CHECK(make_ticket(table, &ticket) == 0, "logical ticket creation");
    memset(&event_storage, 0xa5, sizeof(event_storage));
    memset(&fact_storage, 0xa5, sizeof(fact_storage));

    CHECK(lccf_representation_publish(
              LCCF_REP_CANONICAL_HELPER, &ticket, NULL,
              &helper_counters) == 0,
          "helper publication");
    CHECK(helper_counters.normalization_calls == 0U &&
              helper_counters.site_lookups == 0U,
          "helper publication defers all stable work");

    CHECK(lccf_representation_publish(
              LCCF_REP_SHARED_EVENT, &ticket, &event_storage,
              &event_counters) == 0,
          "event publication");
    CHECK(event_counters.normalization_calls == 1U &&
              event_counters.site_lookups == 0U,
          "event publication normalizes without lookup");
    event_snapshot = event_storage;

    CHECK(lccf_representation_publish(
              LCCF_REP_FULL_FACT, &ticket, &fact_storage,
              &full_counters) == 0,
          "fact publication");
    CHECK(full_counters.normalization_calls == 1U &&
              full_counters.site_lookups == 1U,
          "fact publication normalizes and resolves initial site");
    fact_snapshot = fact_storage;

    CHECK(lccf_representation_materialize(
              LCCF_REP_CANONICAL_HELPER, &ticket, NULL, 2U,
              &helper_counters, &helper_fact) == 0,
          "helper initial materialization");
    CHECK(lccf_representation_materialize(
              LCCF_REP_SHARED_EVENT, &ticket, &event_storage, 2U,
              &event_counters, &event_fact) == 0,
          "event initial materialization");
    CHECK(lccf_representation_materialize(
              LCCF_REP_FULL_FACT, &ticket, &fact_storage, 2U,
              &full_counters, &full_fact) == 0,
          "fact initial materialization");
    CHECK(event_equal(&helper_fact, &event_fact) &&
              event_equal(&event_fact, &full_fact),
          "all representations materialize the same initial fact");

    CHECK(lccf_representation_materialize(
              LCCF_REP_CANONICAL_HELPER, &ticket, NULL, 2U,
              &helper_counters, &helper_fact) == 0 &&
              lccf_representation_materialize(
                  LCCF_REP_SHARED_EVENT, &ticket, &event_storage, 2U,
                  &event_counters, &event_fact) == 0 &&
              lccf_representation_materialize(
                  LCCF_REP_FULL_FACT, &ticket, &fact_storage, 2U,
                  &full_counters, &full_fact) == 0,
          "repeat initial materialization");
    CHECK(helper_counters.normalization_calls == 2U &&
              helper_counters.site_lookups == 2U,
          "helper repeats normalization and lookup");
    CHECK(event_counters.normalization_calls == 1U &&
              event_counters.site_lookups == 2U,
          "event reuses normalization only");
    CHECK(full_counters.normalization_calls == 1U &&
              full_counters.site_lookups == 1U,
          "fact reuses initial descriptor");

    CHECK(lccf_representation_materialize(
              LCCF_REP_CANONICAL_HELPER, &ticket, NULL, 6U,
              &helper_counters, &helper_fact) == 0 &&
              lccf_representation_materialize(
                  LCCF_REP_SHARED_EVENT, &ticket, &event_storage, 6U,
                  &event_counters, &event_fact) == 0 &&
              lccf_representation_materialize(
                  LCCF_REP_FULL_FACT, &ticket, &fact_storage, 6U,
                  &full_counters, &full_fact) == 0,
          "changed continuation materialization");
    CHECK(event_equal(&helper_fact, &event_fact) &&
              event_equal(&event_fact, &full_fact),
          "all representations resolve the same continuation site");
    CHECK(helper_counters.normalization_calls == 3U &&
              helper_counters.site_lookups == 3U &&
              event_counters.normalization_calls == 1U &&
              event_counters.site_lookups == 3U &&
              full_counters.normalization_calls == 1U &&
              full_counters.site_lookups == 2U,
          "representation work equations");
    CHECK(memcmp(&event_storage, &event_snapshot,
                 sizeof(event_storage)) == 0,
          "published event remains immutable");
    CHECK(memcmp(&fact_storage, &fact_snapshot,
                 sizeof(fact_storage)) == 0,
          "published fact remains immutable");
    return 0;
}

static int test_invalid_inputs(void) {
    lccf_fact_site_descriptor_t descriptors[8];
    const lccf_fact_site_descriptor_t *table[8];
    lccf_fact_ticket_t ticket;
    lccf_event_core_t event_storage;
    lccf_fact_core_t fact;
    lccf_fact_counters_t counters = {0};

    make_sites(descriptors, table);
    CHECK(make_ticket(table, &ticket) == 0, "invalid-case ticket");
    CHECK(lccf_representation_publish(
              LCCF_REP_SHARED_EVENT, &ticket, NULL, &counters) != 0,
          "event publication rejects null storage");
    CHECK(lccf_representation_publish(
              LCCF_REP_COUNT, &ticket, &event_storage, &counters) != 0,
          "publication rejects invalid representation");
    CHECK(lccf_representation_materialize(
              LCCF_REP_SHARED_EVENT, &ticket, &event_storage, 8U,
              &counters, &fact) != 0,
          "materialization rejects invalid site");
    return 0;
}

int main(void) {
    if (test_layout_contracts() != 0 ||
        test_work_separation_and_immutability() != 0 ||
        test_invalid_inputs() != 0) {
        return 1;
    }
    puts("[test_lccf_representation] all checks passed");
    return 0;
}
