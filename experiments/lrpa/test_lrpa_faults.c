/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lrpa_internal.h"

#include <stdio.h>
#include <string.h>

#if !defined(LRPA_ENABLE_FAULTS)
#error "test_lrpa_faults must be compiled with LRPA_ENABLE_FAULTS=1"
#endif

#define TEST_CHECK(condition)                                                   \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return false;                                                       \
        }                                                                       \
    } while (0)

static lrpa_manifest_t
fault_manifest(lrpa_fault_t fault)
{
    lrpa_manifest_t manifest;
    uint32_t lane;

    memset(&manifest, 0, sizeof(manifest));
    manifest.version = LRPA_MANIFEST_VERSION;
    manifest.gadget = LRPA_GADGET_SELECT_COMPLETION;
    manifest.coupling = LRPA_COUPLING_INDEPENDENT;
    manifest.lane_count = 4U;
    manifest.worker_count = 4U;
    manifest.rounds = 4U;
    manifest.queue_capacity = 4096U;
    manifest.fault_id = fault;
    manifest.allowed_outcomes = LRPA_ALLOW_ALL;
    manifest.seed = 3U;
    manifest.timeout_ns = UINT64_C(2000000000);
    for (lane = 0U; lane < manifest.lane_count; ++lane) {
        manifest.object_ids[lane] = 100U + lane;
    }
    return manifest;
}

static bool
run_fault_twice(lrpa_fault_t fault, uint32_t expected_round,
                lrpa_oracle_id_t expected_oracle,
                uint64_t expected_signature)
{
    lrpa_manifest_t manifest = fault_manifest(fault);
    lrpa_run_options_t options = {UINT32_MAX, UINT32_MAX};
    uint64_t first_signature = 0U;
    unsigned int repetition;

    TEST_CHECK(lrpa_manifest_validate(&manifest) == LRPA_STATUS_OK);
    for (repetition = 0U; repetition < 2U; ++repetition) {
        lrpa_context_t context;
        lrpa_result_t result;

        TEST_CHECK(lrpa_context_init(&context, &manifest, &options) ==
                   LRPA_STATUS_OK);
        TEST_CHECK(lrpa_context_run(&context, &result) ==
                   LRPA_STATUS_ORACLE_FAILURE);
        TEST_CHECK(result.status == LRPA_STATUS_ORACLE_FAILURE);
        TEST_CHECK(result.failures == 1U);
        TEST_CHECK(result.first_failure_round == expected_round);
        TEST_CHECK(result.first_failure.oracle == expected_oracle);
        TEST_CHECK(result.signature == expected_signature);
        TEST_CHECK(result.cleanup_complete);
        if (repetition == 0U) {
            first_signature = result.signature;
        } else {
            TEST_CHECK(result.signature == first_signature);
        }
        lrpa_context_destroy(&context);
    }
    return true;
}

static bool
test_skipped_winner_cas_is_detected(void)
{
    return run_fault_twice(
        LRPA_FAULT_SELECT_SKIP_WINNER_CAS, 0U,
        LRPA_ORACLE_EXACTLY_ONE_WINNER,
        UINT64_C(0x9644cd826e5dc032));
}

static bool
test_stale_generation_reuse_is_detected(void)
{
    return run_fault_twice(
        LRPA_FAULT_STALE_GENERATION_REUSE, 1U,
        LRPA_ORACLE_STALE_GENERATION,
        UINT64_C(0xd06fdee7a97ed974));
}

static bool
test_non_window_seed_remains_clean(void)
{
    lrpa_manifest_t manifest =
        fault_manifest(LRPA_FAULT_SELECT_SKIP_WINNER_CAS);
    lrpa_run_options_t options = {UINT32_MAX, UINT32_MAX};
    lrpa_context_t context;
    lrpa_result_t result;

    manifest.seed = 1U;
    TEST_CHECK(lrpa_context_init(&context, &manifest, &options) ==
               LRPA_STATUS_OK);
    TEST_CHECK(lrpa_context_run(&context, &result) == LRPA_STATUS_OK);
    TEST_CHECK(result.failures == 0U);
    TEST_CHECK(result.cleanup_complete);
    lrpa_context_destroy(&context);
    return true;
}

int
main(void)
{
    if (!test_skipped_winner_cas_is_detected()) {
        return 1;
    }
    printf("PASS skipped_winner_cas_is_detected\n");
    if (!test_stale_generation_reuse_is_detected()) {
        return 1;
    }
    printf("PASS stale_generation_reuse_is_detected\n");
    if (!test_non_window_seed_remains_clean()) {
        return 1;
    }
    printf("PASS non_window_seed_remains_clean\n");
    printf("LRPA fault calibration tests passed (3 cases)\n");
    return 0;
}
