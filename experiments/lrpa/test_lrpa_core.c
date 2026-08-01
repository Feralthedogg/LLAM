/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lrpa_internal.h"

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

static lrpa_manifest_t
valid_manifest(void)
{
    lrpa_manifest_t manifest;
    uint32_t lane;

    memset(&manifest, 0, sizeof(manifest));
    manifest.version = LRPA_MANIFEST_VERSION;
    manifest.gadget = LRPA_GADGET_SELECT_COMPLETION;
    manifest.coupling = LRPA_COUPLING_RING;
    manifest.lane_count = 4U;
    manifest.worker_count = 4U;
    manifest.rounds = 8U;
    manifest.queue_capacity = 64U;
    manifest.fault_id = LRPA_FAULT_NONE;
    manifest.allowed_outcomes = LRPA_ALLOW_ALL;
    manifest.seed = UINT64_C(0x123456789abcdef0);
    manifest.timeout_ns = UINT64_C(2000000000);
    for (lane = 0U; lane < manifest.lane_count; ++lane) {
        manifest.object_ids[lane] = 100U + lane;
    }
    return manifest;
}

static bool
test_manifest_dimensions_and_enums(void)
{
    lrpa_manifest_t manifest = valid_manifest();

    TEST_CHECK(lrpa_manifest_validate(&manifest) == LRPA_STATUS_OK);
    manifest.version = 99U;
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_INVALID_VERSION);
    manifest = valid_manifest();
    manifest.lane_count = 0U;
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_INVALID_DIMENSIONS);
    manifest = valid_manifest();
    manifest.lane_count = LRPA_MAX_LANES + 1U;
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_INVALID_DIMENSIONS);
    manifest = valid_manifest();
    manifest.worker_count = LRPA_MAX_WORKERS + 1U;
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_INVALID_DIMENSIONS);
    manifest = valid_manifest();
    manifest.rounds = 0U;
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_INVALID_DIMENSIONS);
    manifest = valid_manifest();
    manifest.queue_capacity = LRPA_MAX_TRACE_CAPACITY + 1U;
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_INVALID_DIMENSIONS);
    manifest = valid_manifest();
    manifest.gadget = LRPA_GADGET_COUNT;
    TEST_CHECK(lrpa_manifest_validate(&manifest) == LRPA_STATUS_INVALID_ENUM);
    manifest = valid_manifest();
    manifest.coupling = LRPA_COUPLING_SHARED_SHARD;
    TEST_CHECK(lrpa_manifest_validate(&manifest) == LRPA_STATUS_INVALID_ENUM);
    manifest = valid_manifest();
    manifest.allowed_outcomes = 1U << 31U;
    TEST_CHECK(lrpa_manifest_validate(&manifest) == LRPA_STATUS_INVALID_ENUM);
    return true;
}

static bool
test_manifest_identity_and_perturbation_rules(void)
{
    lrpa_manifest_t manifest = valid_manifest();

    manifest.object_ids[2] = manifest.object_ids[1];
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_DUPLICATE_OBJECT);

    manifest = valid_manifest();
    manifest.perturbation_count = 1U;
    manifest.perturbations[0].kind = LRPA_STEP_AFFINITY_ROTATE;
    manifest.perturbations[0].lane_mask = 1U;
    manifest.perturbations[0].sequence = 0U;
    manifest.perturbation_hash = lrpa_perturbation_hash(
        manifest.perturbations, manifest.perturbation_count);
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_MALFORMED_PERTURBATION);

    manifest = valid_manifest();
    manifest.perturbation_count = 1U;
    manifest.perturbations[0].kind = LRPA_STEP_YIELD;
    manifest.perturbations[0].lane_mask = UINT64_C(1) << 40U;
    manifest.perturbations[0].sequence = 0U;
    manifest.perturbation_hash = lrpa_perturbation_hash(
        manifest.perturbations, manifest.perturbation_count);
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_MALFORMED_PERTURBATION);

    manifest = valid_manifest();
    manifest.perturbation_count = 1U;
    manifest.perturbations[0].kind = LRPA_STEP_SPIN;
    manifest.perturbations[0].lane_mask = 1U;
    manifest.perturbations[0].sequence = 7U;
    manifest.perturbations[0].value = 5;
    manifest.perturbation_hash = UINT64_C(0xfeedface);
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_MALFORMED_PERTURBATION);

    manifest = valid_manifest();
    manifest.fault_id = LRPA_FAULT_SELECT_SKIP_WINNER_CAS;
    TEST_CHECK(lrpa_manifest_validate(&manifest) ==
               LRPA_STATUS_FAULT_UNAVAILABLE);
    return true;
}

static bool
test_checked_size_overflow(void)
{
    size_t result = 99U;

    TEST_CHECK(lrpa_checked_multiply_size(12U, 7U, &result));
    TEST_CHECK(result == 84U);
    TEST_CHECK(!lrpa_checked_multiply_size(SIZE_MAX, 2U, &result));
    TEST_CHECK(result == 0U);
    TEST_CHECK(!lrpa_checked_multiply_size(1U, 1U, NULL));
    return true;
}

static bool
test_lane_state_machine(void)
{
    static const lrpa_lane_phase_t states[] = {
        LRPA_LANE_ALLOCATED,
        LRPA_LANE_SETUP,
        LRPA_LANE_ARMED,
        LRPA_LANE_RELEASED,
        LRPA_LANE_RACING,
        LRPA_LANE_DRAINING,
        LRPA_LANE_VERIFIED,
        LRPA_LANE_DESTROYED,
    };
    lrpa_lane_t lane;
    size_t index;

    lrpa_lane_init(&lane, 7U, 11U);
    TEST_CHECK(lane.id == 7U);
    TEST_CHECK(atomic_load(&lane.phase) == LRPA_LANE_ALLOCATED);
    TEST_CHECK(lrpa_lane_transition(&lane, LRPA_LANE_ALLOCATED,
                                    LRPA_LANE_ARMED) ==
               LRPA_STATUS_INVALID_TRANSITION);
    for (index = 1U; index < sizeof(states) / sizeof(states[0]); ++index) {
        TEST_CHECK(lrpa_lane_transition(&lane, states[index - 1U],
                                        states[index]) == LRPA_STATUS_OK);
    }
    TEST_CHECK(lrpa_lane_transition(&lane, LRPA_LANE_DESTROYED,
                                    LRPA_LANE_SETUP) ==
               LRPA_STATUS_INVALID_TRANSITION);
    return true;
}

static bool
test_trace_is_bounded(void)
{
    lrpa_trace_entry_t entries[2];
    lrpa_trace_entry_t event;
    lrpa_trace_t trace;

    memset(entries, 0xa5, sizeof(entries));
    memset(&event, 0, sizeof(event));
    event.sequence = 1U;
    event.lane = 3U;
    event.event_kind = 9U;
    lrpa_trace_bind(&trace, entries, 2U);
    TEST_CHECK(lrpa_trace_record(&trace, &event));
    event.sequence = 2U;
    TEST_CHECK(lrpa_trace_record(&trace, &event));
    event.sequence = 3U;
    TEST_CHECK(!lrpa_trace_record(&trace, &event));
    TEST_CHECK(atomic_load(&trace.next) == 3U);
    TEST_CHECK(atomic_load(&trace.truncated));
    TEST_CHECK(entries[0].sequence == 1U);
    TEST_CHECK(entries[1].sequence == 2U);
    return true;
}

static bool
test_signature_is_semantic_and_stable(void)
{
    lrpa_manifest_t manifest = valid_manifest();
    lrpa_failure_t first;
    lrpa_failure_t second;
    uint64_t signature;

    memset(&first, 0, sizeof(first));
    first.oracle = LRPA_ORACLE_EXACTLY_ONE_WINNER;
    first.expected = 1U;
    first.actual = 2U;
    first.generation_first = 9U;
    first.generation_second = 10U;
    first.object_id = 4U;
    first.timestamp_ns = 123U;
    first.debug_address = 0x1000U;
    second = first;
    second.timestamp_ns = UINT64_MAX;
    second.debug_address = 0xf000U;

    signature = lrpa_failure_signature(&manifest, &first);
    TEST_CHECK(signature == UINT64_C(0x3aac767aafb94b32));
    TEST_CHECK(lrpa_failure_signature(&manifest, &second) == signature);
    second.actual = 3U;
    TEST_CHECK(lrpa_failure_signature(&manifest, &second) != signature);
    return true;
}

static bool
test_coordination_releases_only_after_all_actors_arm(void)
{
    lrpa_manifest_t manifest = valid_manifest();
    lrpa_run_options_t options = {UINT32_MAX, UINT32_MAX};
    lrpa_context_t context;
    lrpa_result_t result;
    uint32_t lane;

    manifest.worker_count = 2U;
    manifest.rounds = 3U;
    manifest.queue_capacity = 1024U;
    TEST_CHECK(lrpa_context_init(&context, &manifest, &options) ==
               LRPA_STATUS_OK);
    TEST_CHECK(lrpa_context_run_coordination_probe(&context, &result) ==
               LRPA_STATUS_OK);
    TEST_CHECK(result.status == LRPA_STATUS_OK);
    TEST_CHECK(result.rounds_completed == manifest.rounds);
    TEST_CHECK(result.cleanup_complete);
    TEST_CHECK(atomic_load(&context.release_observed_armed) ==
               context.actor_count);
    for (lane = 0U; lane < manifest.lane_count; ++lane) {
        TEST_CHECK(atomic_load(&context.lanes[lane].phase) ==
                   LRPA_LANE_VERIFIED);
    }

    TEST_CHECK(lrpa_context_reset(&context) == LRPA_STATUS_OK);
    TEST_CHECK(atomic_load(&context.trace.next) == 0U);
    for (lane = 0U; lane < manifest.lane_count; ++lane) {
        TEST_CHECK(atomic_load(&context.lanes[lane].phase) ==
                   LRPA_LANE_ALLOCATED);
        TEST_CHECK(atomic_load(&context.lanes[lane].winner_count) == 0U);
        TEST_CHECK(atomic_load(&context.lanes[lane].terminal_count) == 0U);
        TEST_CHECK(atomic_load(&context.lanes[lane].invariant_failures) ==
                   0U);
    }
    TEST_CHECK(lrpa_context_run_coordination_probe(&context, &result) ==
               LRPA_STATUS_OK);
    TEST_CHECK(result.cleanup_complete);
    lrpa_context_destroy(&context);
    return true;
}

static bool
test_setup_failure_aborts_and_joins_started_actors(void)
{
    lrpa_manifest_t manifest = valid_manifest();
    lrpa_run_options_t options = {2U, UINT32_MAX};
    lrpa_context_t context;
    lrpa_result_t result;
    uint32_t actor;

    TEST_CHECK(lrpa_context_init(&context, &manifest, &options) ==
               LRPA_STATUS_OK);
    TEST_CHECK(lrpa_context_run_coordination_probe(&context, &result) ==
               LRPA_STATUS_PLATFORM_ERROR);
    TEST_CHECK(result.status == LRPA_STATUS_PLATFORM_ERROR);
    TEST_CHECK(result.cleanup_complete);
    for (actor = 0U; actor < context.actor_count; ++actor) {
        TEST_CHECK(!context.actors[actor].started ||
                   context.actors[actor].joined);
    }
    lrpa_context_destroy(&context);
    return true;
}

static bool
test_timeout_breaks_barriers_and_joins_every_actor(void)
{
    lrpa_manifest_t manifest = valid_manifest();
    lrpa_run_options_t options = {UINT32_MAX, 0U};
    lrpa_context_t context;
    lrpa_result_t result;
    uint32_t actor;

    manifest.timeout_ns = UINT64_C(20000000);
    TEST_CHECK(lrpa_context_init(&context, &manifest, &options) ==
               LRPA_STATUS_OK);
    TEST_CHECK(lrpa_context_run_coordination_probe(&context, &result) ==
               LRPA_STATUS_TIMEOUT);
    TEST_CHECK(result.status == LRPA_STATUS_TIMEOUT);
    TEST_CHECK(result.cleanup_complete);
    for (actor = 0U; actor < context.actor_count; ++actor) {
        TEST_CHECK(!context.actors[actor].started ||
                   context.actors[actor].joined);
    }
    lrpa_context_destroy(&context);
    return true;
}

static bool
test_coordination_trace_overflow_is_reported(void)
{
    lrpa_manifest_t manifest = valid_manifest();
    lrpa_run_options_t options = {UINT32_MAX, UINT32_MAX};
    lrpa_context_t context;
    lrpa_result_t result;

    manifest.queue_capacity = 2U;
    manifest.rounds = 2U;
    TEST_CHECK(lrpa_context_init(&context, &manifest, &options) ==
               LRPA_STATUS_OK);
    TEST_CHECK(lrpa_context_run_coordination_probe(&context, &result) ==
               LRPA_STATUS_OK);
    TEST_CHECK(result.trace_entries == 2U);
    TEST_CHECK(result.trace_truncated);
    TEST_CHECK(result.cleanup_complete);
    lrpa_context_destroy(&context);
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
        {"manifest_dimensions_and_enums", test_manifest_dimensions_and_enums},
        {"manifest_identity_and_perturbation_rules",
         test_manifest_identity_and_perturbation_rules},
        {"checked_size_overflow", test_checked_size_overflow},
        {"lane_state_machine", test_lane_state_machine},
        {"trace_is_bounded", test_trace_is_bounded},
        {"signature_is_semantic_and_stable",
         test_signature_is_semantic_and_stable},
        {"coordination_releases_only_after_all_actors_arm",
         test_coordination_releases_only_after_all_actors_arm},
        {"setup_failure_aborts_and_joins_started_actors",
         test_setup_failure_aborts_and_joins_started_actors},
        {"timeout_breaks_barriers_and_joins_every_actor",
         test_timeout_breaks_barriers_and_joins_every_actor},
        {"coordination_trace_overflow_is_reported",
         test_coordination_trace_overflow_is_reported},
    };
    size_t index;

    for (index = 0U; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        if (!tests[index].function()) {
            fprintf(stderr, "FAIL %s\n", tests[index].name);
            return 1;
        }
        printf("PASS %s\n", tests[index].name);
    }
    printf("LRPA phase0 contract tests passed (%zu cases)\n",
           sizeof(tests) / sizeof(tests[0]));
    return 0;
}
