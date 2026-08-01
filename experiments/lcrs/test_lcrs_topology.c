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

int main(void) {
    if (test_default_config() != 0 || test_invalid_create_contract() != 0 ||
        test_literal_local_rows() != 0 ||
        test_remote_rows_respect_locality() != 0 ||
        test_local_properties_through_512() != 0 ||
        test_remote_coverage_representative_sizes() != 0 ||
        test_deterministic_construction() != 0) {
        return 1;
    }
    puts("[test_lcrs_topology] all checks passed");
    return 0;
}
