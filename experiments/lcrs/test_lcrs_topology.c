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

int main(void) {
    if (test_default_config() != 0 || test_invalid_create_contract() != 0 ||
        test_literal_local_rows() != 0 ||
        test_remote_rows_respect_locality() != 0) {
        return 1;
    }
    puts("[test_lcrs_topology] all checks passed");
    return 0;
}
