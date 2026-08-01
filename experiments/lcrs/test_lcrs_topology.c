// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

#include "lcrs_model.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *message) {
    fprintf(stderr, "[test_lcrs_topology] %s\n", message);
    return 1;
}

static int test_default_config(void) {
    lcrs_model_config_t config;

    memset(&config, 0, sizeof(config));
    lcrs_model_config_default(&config);
    if (config.palette_epochs != 16U || config.local_width != 4U ||
        config.remote_width != 4U || config.small_runtime_cutoff != 16U ||
        config.fullscan_miss_limit != 2U) {
        return fail("default policy bounds changed");
    }
    return 0;
}

static int test_invalid_create_contract(void) {
    const lcrs_model_shard_desc_t one[] = {{0U, 0U}};
    const lcrs_model_shard_desc_t duplicate[] = {{0U, 0U}, {0U, 1U}};
    const lcrs_model_shard_desc_t sparse[] = {{0U, 0U}, {2U, 0U}};
    lcrs_model_config_t config;
    lcrs_model_topology_t *topology = (lcrs_model_topology_t *)(uintptr_t)1U;

    lcrs_model_config_default(&config);
    if (lcrs_model_topology_create(NULL, 1U, &config, &topology) != EINVAL ||
        topology != NULL) {
        return fail("null shard table must fail closed");
    }
    topology = (lcrs_model_topology_t *)(uintptr_t)1U;
    if (lcrs_model_topology_create(one, 0U, &config, &topology) != EINVAL ||
        topology != NULL) {
        return fail("empty shard table must fail closed");
    }
    topology = (lcrs_model_topology_t *)(uintptr_t)1U;
    if (lcrs_model_topology_create(one, 1U, NULL, &topology) != EINVAL ||
        topology != NULL) {
        return fail("null config must fail closed");
    }
    if (lcrs_model_topology_create(one, 1U, &config, NULL) != EINVAL) {
        return fail("null result pointer must fail");
    }
    topology = (lcrs_model_topology_t *)(uintptr_t)1U;
    if (lcrs_model_topology_create(duplicate,
                                   sizeof(duplicate) / sizeof(duplicate[0]),
                                   &config,
                                   &topology) != EINVAL ||
        topology != NULL) {
        return fail("duplicate shard identity must fail closed");
    }
    topology = (lcrs_model_topology_t *)(uintptr_t)1U;
    if (lcrs_model_topology_create(sparse,
                                   sizeof(sparse) / sizeof(sparse[0]),
                                   &config,
                                   &topology) != EINVAL ||
        topology != NULL) {
        return fail("sparse shard identity must fail closed");
    }
    config.local_width = 5U;
    config.remote_width = 4U;
    topology = (lcrs_model_topology_t *)(uintptr_t)1U;
    if (lcrs_model_topology_create(one, 1U, &config, &topology) != EINVAL ||
        topology != NULL) {
        return fail("candidate width overflow must fail closed");
    }
    return 0;
}

static int test_literal_local_rows(void) {
    const lcrs_model_shard_desc_t shards[] = {
        {0U, 7U}, {1U, 7U}, {2U, 7U}, {3U, 7U},
    };
    lcrs_model_config_t config;
    lcrs_model_topology_t *topology = NULL;
    lcrs_model_topology_info_t info;
    const lcrs_model_row_t *row;

    lcrs_model_config_default(&config);
    config.local_width = 2U;
    config.remote_width = 0U;
    config.runtime_id = 0U;
    if (lcrs_model_topology_create(shards,
                                   sizeof(shards) / sizeof(shards[0]),
                                   &config,
                                   &topology) != 0) {
        return fail("four-shard topology create");
    }
    row = lcrs_model_topology_row(topology, 0U, 0U);
    if (row == NULL || row->local_count != 2U || row->remote_count != 0U ||
        row->candidate_ids[0] != 1U || row->candidate_ids[1] != 2U) {
        lcrs_model_topology_destroy(topology);
        return fail("epoch zero local rotation changed");
    }
    row = lcrs_model_topology_row(topology, 0U, 1U);
    if (row == NULL || row->local_count != 2U ||
        row->candidate_ids[0] != 3U || row->candidate_ids[1] != 1U) {
        lcrs_model_topology_destroy(topology);
        return fail("epoch one local rotation changed");
    }
    if (lcrs_model_topology_info(topology, &info) != 0 ||
        info.version != 1U || info.shard_count != 4U || info.node_count != 1U ||
        info.max_local_indegree != 2U || info.max_remote_indegree != 0U ||
        info.allocation_count == 0U ||
        lcrs_model_topology_validate(topology) != 0) {
        lcrs_model_topology_destroy(topology);
        return fail("topology metadata or validation");
    }
    lcrs_model_topology_destroy(topology);
    return 0;
}

static int test_remote_rows_respect_locality(void) {
    const lcrs_model_shard_desc_t shards[] = {
        {0U, 4U}, {1U, 4U}, {2U, 9U}, {3U, 9U}, {4U, 9U},
    };
    lcrs_model_config_t config;
    lcrs_model_topology_t *topology = NULL;
    lcrs_model_topology_info_t info;
    uint32_t epoch;
    uint32_t thief;

    lcrs_model_config_default(&config);
    config.local_width = 2U;
    config.remote_width = 3U;
    if (lcrs_model_topology_create(shards,
                                   sizeof(shards) / sizeof(shards[0]),
                                   &config,
                                   &topology) != 0) {
        return fail("uneven topology create");
    }
    for (epoch = 0U; epoch < 16U; ++epoch) {
        for (thief = 0U; thief < 5U; ++thief) {
            const lcrs_model_row_t *row =
                lcrs_model_topology_row(topology, thief, epoch);
            uint32_t i;
            uint32_t j;

            if (row == NULL) {
                lcrs_model_topology_destroy(topology);
                return fail("missing topology row");
            }
            for (i = 0U; i < (uint32_t)row->local_count; ++i) {
                const uint32_t candidate = row->candidate_ids[i];
                if (candidate == thief ||
                    lcrs_model_topology_node(topology, candidate) !=
                        lcrs_model_topology_node(topology, thief)) {
                    lcrs_model_topology_destroy(topology);
                    return fail("local row escaped locality or selected self");
                }
            }
            for (i = 0U; i < (uint32_t)row->remote_count; ++i) {
                const uint32_t index = (uint32_t)row->local_count + i;
                const uint32_t candidate = row->candidate_ids[index];
                if (lcrs_model_topology_node(topology, candidate) ==
                    lcrs_model_topology_node(topology, thief)) {
                    lcrs_model_topology_destroy(topology);
                    return fail("remote row stayed in locality");
                }
            }
            for (i = 0U;
                 i < (uint32_t)row->local_count + (uint32_t)row->remote_count;
                 ++i) {
                for (j = i + 1U;
                     j < (uint32_t)row->local_count +
                             (uint32_t)row->remote_count;
                     ++j) {
                    if (row->candidate_ids[i] == row->candidate_ids[j]) {
                        lcrs_model_topology_destroy(topology);
                        return fail("candidate row contains a duplicate");
                    }
                }
            }
        }
    }
    if (lcrs_model_topology_info(topology, &info) != 0 ||
        info.max_remote_indegree != 3U) {
        lcrs_model_topology_destroy(topology);
        return fail("uneven remote indegree bound changed");
    }
    lcrs_model_topology_destroy(topology);
    return 0;
}

static int verify_complete_coverage(const lcrs_model_topology_t *topology,
                                    uint32_t shard_count,
                                    int remote) {
    lcrs_model_topology_info_t info;
    uint32_t thief;

    if (lcrs_model_topology_info(topology, &info) != 0) {
        return fail("coverage metadata");
    }
    for (thief = 0U; thief < shard_count; ++thief) {
        uint8_t seen[512];
        uint32_t epoch;
        uint32_t candidate;

        memset(seen, 0, sizeof(seen));
        for (epoch = 0U; epoch < info.epoch_count; ++epoch) {
            const lcrs_model_row_t *row =
                lcrs_model_topology_row(topology, thief, epoch);
            const uint32_t start = remote ? (uint32_t)row->local_count : 0U;
            const uint32_t count = remote ? (uint32_t)row->remote_count
                                          : (uint32_t)row->local_count;
            uint32_t slot;

            for (slot = 0U; slot < count; ++slot) {
                seen[row->candidate_ids[start + slot]] = 1U;
            }
        }
        for (candidate = 0U; candidate < shard_count; ++candidate) {
            const int same_node =
                lcrs_model_topology_node(topology, candidate) ==
                lcrs_model_topology_node(topology, thief);
            const int expected = candidate != thief &&
                                 ((remote != 0 && same_node == 0) ||
                                  (remote == 0 && same_node != 0));

            if ((seen[candidate] != 0U) != expected) {
                return fail(remote ? "remote palette lost peer coverage"
                                   : "local palette lost peer coverage");
            }
        }
    }
    return 0;
}

static int test_local_properties_through_512(void) {
    lcrs_model_shard_desc_t shards[512];
    uint32_t shard_count;

    for (shard_count = 1U; shard_count <= 512U; ++shard_count) {
        lcrs_model_config_t config;
        lcrs_model_topology_t *topology = NULL;
        lcrs_model_topology_info_t info;
        uint32_t shard;

        for (shard = 0U; shard < shard_count; ++shard) {
            shards[shard].shard_id = shard;
            shards[shard].node_id = 3U;
        }
        lcrs_model_config_default(&config);
        config.remote_width = 0U;
        config.runtime_id = UINT64_C(0x1234);
        if (lcrs_model_topology_create(shards,
                                       shard_count,
                                       &config,
                                       &topology) != 0 ||
            lcrs_model_topology_validate(topology) != 0 ||
            verify_complete_coverage(topology, shard_count, 0) != 0 ||
            lcrs_model_topology_info(topology, &info) != 0 ||
            info.max_local_indegree > config.local_width) {
            lcrs_model_topology_destroy(topology);
            return fail("local exhaustive topology property");
        }
        lcrs_model_topology_destroy(topology);
    }
    return 0;
}

static int test_remote_coverage_representative_sizes(void) {
    static const uint32_t sizes[] = {
        2U, 3U, 4U, 5U, 7U, 8U, 16U, 31U, 32U,
        33U, 63U, 64U, 127U, 128U, 255U, 256U, 511U, 512U,
    };
    lcrs_model_shard_desc_t shards[512];
    size_t case_index;

    for (case_index = 0U; case_index < sizeof(sizes) / sizeof(sizes[0]);
         ++case_index) {
        const uint32_t shard_count = sizes[case_index];
        const uint32_t split = (shard_count + 2U) / 3U;
        lcrs_model_config_t config;
        lcrs_model_topology_t *topology = NULL;
        uint32_t shard;

        for (shard = 0U; shard < shard_count; ++shard) {
            shards[shard].shard_id = shard;
            shards[shard].node_id = shard < split ? 10U : 20U;
        }
        lcrs_model_config_default(&config);
        config.local_width = 0U;
        config.runtime_id = UINT64_C(0xfeedface);
        if (lcrs_model_topology_create(shards,
                                       shard_count,
                                       &config,
                                       &topology) != 0 ||
            lcrs_model_topology_validate(topology) != 0 ||
            verify_complete_coverage(topology, shard_count, 1) != 0) {
            lcrs_model_topology_destroy(topology);
            return fail("remote representative topology property");
        }
        lcrs_model_topology_destroy(topology);
    }
    return 0;
}

static int test_deterministic_construction(void) {
    lcrs_model_shard_desc_t shards[17];
    lcrs_model_config_t config;
    lcrs_model_topology_t *left = NULL;
    lcrs_model_topology_t *right = NULL;
    lcrs_model_topology_info_t info;
    uint32_t shard;
    uint32_t epoch;

    for (shard = 0U; shard < 17U; ++shard) {
        shards[shard].shard_id = shard;
        shards[shard].node_id = shard < 3U ? 1U : (shard < 11U ? 4U : 9U);
    }
    lcrs_model_config_default(&config);
    config.runtime_id = UINT64_C(42);
    if (lcrs_model_topology_create(shards, 17U, &config, &left) != 0 ||
        lcrs_model_topology_create(shards, 17U, &config, &right) != 0 ||
        lcrs_model_topology_info(left, &info) != 0) {
        lcrs_model_topology_destroy(right);
        lcrs_model_topology_destroy(left);
        return fail("deterministic topology create");
    }
    for (epoch = 0U; epoch < info.epoch_count; ++epoch) {
        for (shard = 0U; shard < info.shard_count; ++shard) {
            const lcrs_model_row_t *left_row =
                lcrs_model_topology_row(left, shard, epoch);
            const lcrs_model_row_t *right_row =
                lcrs_model_topology_row(right, shard, epoch);

            if (memcmp(left_row, right_row, sizeof(*left_row)) != 0) {
                lcrs_model_topology_destroy(right);
                lcrs_model_topology_destroy(left);
                return fail("topology construction is not deterministic");
            }
        }
    }
    lcrs_model_topology_destroy(right);
    lcrs_model_topology_destroy(left);
    return 0;
}

static void fill_snapshots(lcrs_model_shard_snapshot_t *snapshots,
                           uint32_t count) {
    uint32_t shard;

    for (shard = 0U; shard < count; ++shard) {
        snapshots[shard].shard_id = shard;
        snapshots[shard].depth = 0U;
        snapshots[shard].online = 1U;
        snapshots[shard].paused = 0U;
        snapshots[shard].eligible = 1U;
        snapshots[shard].race_lost = 0U;
    }
}

static int make_single_node_topology(uint32_t shard_count,
                                     uint16_t width,
                                     lcrs_model_topology_t **out) {
    lcrs_model_shard_desc_t shards[32];
    lcrs_model_config_t config;
    uint32_t shard;

    if (shard_count > 32U) {
        return EINVAL;
    }
    for (shard = 0U; shard < shard_count; ++shard) {
        shards[shard].shard_id = shard;
        shards[shard].node_id = 0U;
    }
    lcrs_model_config_default(&config);
    config.local_width = width;
    config.remote_width = 0U;
    config.small_runtime_cutoff = 0U;
    return lcrs_model_topology_create(shards, shard_count, &config, out);
}

static int test_selector_validation_and_names(void) {
    lcrs_model_topology_t *topology = NULL;
    lcrs_model_shard_snapshot_t snapshots[6];
    lcrs_model_select_input_t input;
    lcrs_model_select_result_t result;

    if (make_single_node_topology(6U, 2U, &topology) != 0) {
        return fail("selector validation topology");
    }
    fill_snapshots(snapshots, 6U);
    memset(&input, 0, sizeof(input));
    input.policy = LCRS_MODEL_GLOBAL_DEEPEST;
    input.thief_shard = 0U;
    input.shards = snapshots;
    input.shard_count = 6U;
    memset(&result, 0xa5, sizeof(result));
    if (lcrs_model_select(NULL, &input, &result) != EINVAL ||
        result.victim_shard != LCRS_MODEL_NO_SHARD ||
        lcrs_model_select(topology, NULL, &result) != EINVAL ||
        lcrs_model_select(topology, &input, NULL) != EINVAL) {
        lcrs_model_topology_destroy(topology);
        return fail("selector null input contract");
    }
    input.policy = (lcrs_model_policy_t)99;
    if (lcrs_model_select(topology, &input, &result) != EINVAL) {
        lcrs_model_topology_destroy(topology);
        return fail("selector accepted invalid policy");
    }
    input.policy = LCRS_MODEL_GLOBAL_DEEPEST;
    input.shard_count = 5U;
    if (lcrs_model_select(topology, &input, &result) != EINVAL) {
        lcrs_model_topology_destroy(topology);
        return fail("selector accepted truncated snapshot");
    }
    input.shard_count = 6U;
    snapshots[4].shard_id = 5U;
    if (lcrs_model_select(topology, &input, &result) != EINVAL) {
        lcrs_model_topology_destroy(topology);
        return fail("selector accepted reordered snapshot");
    }
    snapshots[4].shard_id = 4U;
    if (strcmp(lcrs_model_policy_name(LCRS_MODEL_GLOBAL_DEEPEST),
               "global_deepest") != 0 ||
        strcmp(lcrs_model_policy_name(LCRS_MODEL_RANDOM_K),
               "deterministic_random_k") != 0 ||
        strcmp(lcrs_model_policy_name(LCRS_MODEL_CODED_ROTATION),
               "coded_rotation_k") != 0 ||
        strcmp(lcrs_model_policy_name(LCRS_MODEL_CODED_WITH_FULLSCAN),
               "coded_rotation_k_plus_fullscan") != 0 ||
        lcrs_model_policy_name((lcrs_model_policy_t)99) != NULL) {
        lcrs_model_topology_destroy(topology);
        return fail("policy name contract");
    }
    lcrs_model_topology_destroy(topology);
    return 0;
}

static int test_global_and_coded_selection(void) {
    lcrs_model_topology_t *topology = NULL;
    lcrs_model_shard_snapshot_t snapshots[6];
    lcrs_model_select_input_t input;
    lcrs_model_select_result_t result;

    if (make_single_node_topology(6U, 2U, &topology) != 0) {
        return fail("selector topology");
    }
    fill_snapshots(snapshots, 6U);
    snapshots[1].depth = 5U;
    snapshots[2].depth = 9U;
    snapshots[3].depth = 100U;
    memset(&input, 0, sizeof(input));
    input.policy = LCRS_MODEL_GLOBAL_DEEPEST;
    input.thief_shard = 0U;
    input.shards = snapshots;
    input.shard_count = 6U;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.found == 0U || result.victim_shard != 3U ||
        result.probes != 5U || result.used_fullscan == 0U) {
        lcrs_model_topology_destroy(topology);
        return fail("global deepest selection");
    }
    input.policy = LCRS_MODEL_CODED_ROTATION;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.victim_shard != 2U || result.probes != 2U ||
        result.candidate_tries != 1U || result.used_fullscan != 0U) {
        lcrs_model_topology_destroy(topology);
        return fail("coded row depth ordering");
    }
    snapshots[2].race_lost = 1U;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.victim_shard != 1U || result.candidate_tries != 2U) {
        lcrs_model_topology_destroy(topology);
        return fail("candidate race did not fall through");
    }
    lcrs_model_topology_destroy(topology);
    return 0;
}

static int test_coded_fallbacks(void) {
    lcrs_model_topology_t *topology = NULL;
    lcrs_model_shard_snapshot_t snapshots[6];
    lcrs_model_select_input_t input;
    lcrs_model_select_result_t result;

    if (make_single_node_topology(6U, 2U, &topology) != 0) {
        return fail("fallback topology");
    }
    fill_snapshots(snapshots, 6U);
    snapshots[3].depth = 10U;
    snapshots[4].depth = 7U;
    memset(&input, 0, sizeof(input));
    input.policy = LCRS_MODEL_CODED_WITH_FULLSCAN;
    input.thief_shard = 0U;
    input.shards = snapshots;
    input.shard_count = 6U;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.victim_shard != 3U || result.checked_next_epoch == 0U ||
        result.used_fullscan != 0U || result.probes != 4U) {
        lcrs_model_topology_destroy(topology);
        return fail("first miss did not inspect next epoch");
    }
    input.miss_streak = 2U;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.victim_shard != 3U || result.used_fullscan == 0U ||
        result.checked_next_epoch != 0U) {
        lcrs_model_topology_destroy(topology);
        return fail("miss streak did not invoke exact fallback");
    }
    input.miss_streak = 0U;
    input.pressure = 1U;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.victim_shard != 3U || result.used_fullscan == 0U) {
        lcrs_model_topology_destroy(topology);
        return fail("pressure did not invoke exact fallback");
    }
    input.pressure = 0U;
    input.overflow_seen = 1U;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.used_fullscan == 0U) {
        lcrs_model_topology_destroy(topology);
        return fail("overflow did not invoke exact fallback");
    }
    input.overflow_seen = 0U;
    input.deterministic_runtime = 1U;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.found != 0U || result.probes != 0U ||
        result.used_fullscan != 0U) {
        lcrs_model_topology_destroy(topology);
        return fail("deterministic runtime attempted stealing");
    }
    lcrs_model_topology_destroy(topology);
    return 0;
}

static int test_invalid_candidates_and_random_determinism(void) {
    lcrs_model_topology_t *topology = NULL;
    lcrs_model_shard_snapshot_t snapshots[6];
    lcrs_model_select_input_t input;
    lcrs_model_select_result_t first;
    lcrs_model_select_result_t second;

    if (make_single_node_topology(6U, 2U, &topology) != 0) {
        return fail("invalid candidate topology");
    }
    fill_snapshots(snapshots, 6U);
    snapshots[1].online = 0U;
    snapshots[2].paused = 1U;
    snapshots[3].depth = 11U;
    memset(&input, 0, sizeof(input));
    input.policy = LCRS_MODEL_CODED_WITH_FULLSCAN;
    input.thief_shard = 0U;
    input.shards = snapshots;
    input.shard_count = 6U;
    if (lcrs_model_select(topology, &input, &first) != 0 ||
        first.victim_shard != 3U || first.used_fullscan == 0U ||
        first.invalid_candidates < 2U) {
        lcrs_model_topology_destroy(topology);
        return fail("invalid candidate majority did not fall back");
    }
    fill_snapshots(snapshots, 6U);
    snapshots[1].depth = 3U;
    snapshots[2].depth = 4U;
    snapshots[3].depth = 5U;
    snapshots[4].depth = 6U;
    snapshots[5].depth = 7U;
    input.policy = LCRS_MODEL_RANDOM_K;
    input.random_seed = UINT64_C(0xabcdef);
    if (lcrs_model_select(topology, &input, &first) != 0 ||
        lcrs_model_select(topology, &input, &second) != 0 ||
        first.victim_shard != second.victim_shard ||
        first.probes != second.probes || first.probes != 2U ||
        first.used_fullscan != 0U) {
        lcrs_model_topology_destroy(topology);
        return fail("random-k selection is not bounded and deterministic");
    }
    lcrs_model_topology_destroy(topology);
    return 0;
}

static int test_locality_priority_and_small_cutoff(void) {
    const lcrs_model_shard_desc_t shards[] = {
        {0U, 0U}, {1U, 0U}, {2U, 1U}, {3U, 1U},
    };
    lcrs_model_config_t config;
    lcrs_model_topology_t *topology = NULL;
    lcrs_model_shard_snapshot_t snapshots[4];
    lcrs_model_select_input_t input;
    lcrs_model_select_result_t result;
    const lcrs_model_row_t *row;
    uint32_t remote;

    lcrs_model_config_default(&config);
    config.local_width = 1U;
    config.remote_width = 1U;
    config.small_runtime_cutoff = 0U;
    if (lcrs_model_topology_create(shards, 4U, &config, &topology) != 0) {
        return fail("locality priority topology");
    }
    row = lcrs_model_topology_row(topology, 0U, 0U);
    remote = row->candidate_ids[row->local_count];
    fill_snapshots(snapshots, 4U);
    snapshots[1].depth = 1U;
    snapshots[remote].depth = 100U;
    memset(&input, 0, sizeof(input));
    input.policy = LCRS_MODEL_CODED_ROTATION;
    input.thief_shard = 0U;
    input.shards = snapshots;
    input.shard_count = 4U;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.victim_shard != 1U || result.probes != 1U) {
        lcrs_model_topology_destroy(topology);
        return fail("remote depth bypassed viable local candidate");
    }
    snapshots[1].race_lost = 1U;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.victim_shard != remote || result.probes != 2U ||
        result.candidate_tries != 2U) {
        lcrs_model_topology_destroy(topology);
        return fail("remote candidate not tried after local race loss");
    }
    lcrs_model_topology_destroy(topology);

    lcrs_model_config_default(&config);
    config.local_width = 1U;
    config.remote_width = 1U;
    topology = NULL;
    if (lcrs_model_topology_create(shards, 4U, &config, &topology) != 0) {
        return fail("small cutoff topology");
    }
    fill_snapshots(snapshots, 4U);
    snapshots[3].depth = 9U;
    input.policy = LCRS_MODEL_CODED_WITH_FULLSCAN;
    input.shards = snapshots;
    if (lcrs_model_select(topology, &input, &result) != 0 ||
        result.victim_shard != 3U || result.used_fullscan == 0U) {
        lcrs_model_topology_destroy(topology);
        return fail("small runtime did not retain full scan");
    }
    lcrs_model_topology_destroy(topology);
    return 0;
}

int main(void) {
    if (test_default_config() != 0 || test_invalid_create_contract() != 0 ||
        test_literal_local_rows() != 0 ||
        test_remote_rows_respect_locality() != 0 ||
        test_local_properties_through_512() != 0 ||
        test_remote_coverage_representative_sizes() != 0 ||
        test_deterministic_construction() != 0 ||
        test_selector_validation_and_names() != 0 ||
        test_global_and_coded_selection() != 0 ||
        test_coded_fallbacks() != 0 ||
        test_invalid_candidates_and_random_determinism() != 0 ||
        test_locality_priority_and_small_cutoff() != 0) {
        return 1;
    }
    puts("[test_lcrs_topology] all checks passed");
    return 0;
}
